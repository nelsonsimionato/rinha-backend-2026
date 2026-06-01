//go:build ignore

// build_kdtree.go — Phase 2 index builder (KD-tree over i16, scale 5000).
//
// Balanced, EXACT KD-tree over the int16-quantized 16-lane vectors.
// Sliding-midpoint split on the widest-spread dimension (median fallback on
// degenerate splits), leaf size 32. Each node stores its subtree AABB so the
// C runtime prunes with the AVX2 bound_dist_sq kernel.
//
// On-disk format v13 (little-endian):
//   [0]       uint8  formatVersion = 13
//   [1:4]     reserved
//   [4:8]     uint32 N            (total records)
//   [8:12]    uint32 nodeCount
//   [12:16]   reserved
//   [16:..]   nodes[nodeCount] × KDNode (72 bytes):
//                 [0:32]  bmin[16] int16   (subtree AABB min; lanes 14,15 = 0)
//                 [32:64] bmax[16] int16
//                 [64:68] uint32 a         internal: left child idx ; leaf: first record idx
//                 [68:72] uint32 b         internal: right child idx (<2^31)
//                                          leaf:     count | 0x80000000
//   [..]      data[N][16] int16  (reordered into KD leaf order)
//   [..]      isFraud[N]  uint8  (same order)
//
// Build:  go run tools/build_kdtree.go      (reads resources/references.json.gz,
//                                            writes resources/index.bin)
package main

import (
	"compress/gzip"
	"encoding/binary"
	"encoding/json"
	"log"
	"math"
	"os"
	"sort"
)

const (
	Dimensions    = 14
	RecordStride  = 16 // i16 lanes per record (14 dims + 2 zero pad)
	FormatVersion = uint8(14)
	HeaderSize    = 16
	LeafSize      = 32
	NodeBytes     = 72
	I16Scale      = 10000
	I16Sentinel   = int16(-10000)
	LeafFlag      = uint32(0x80000000)
	CountMask     = uint32(0x7fffffff)
)

// quantizeI16 must match the runtime quantize_i16 (vectorize.c) bit-for-bit.
func quantizeI16(x float64) int16 {
	if x < 0.0 {
		return I16Sentinel // null/missing (float -1) -> -5000
	}
	if x > 1.0 {
		return I16Scale
	}
	return int16(math.Round(x * float64(I16Scale)))
}

// Flat point store. pts[i*16 .. i*16+16] is record i (original order).
var (
	pts   []int16
	fraud []uint8
	idx   []int32 // permutation, reordered during build into leaf order

	nodes    []kdNode
	maxDepth int
)

type kdNode struct {
	bmin [RecordStride]int16
	bmax [RecordStride]int16
	a, b uint32
}

func computeBox(lo, hi int) (bmin, bmax [RecordStride]int16) {
	for d := 0; d < RecordStride; d++ {
		bmin[d] = math.MaxInt16
		bmax[d] = math.MinInt16
	}
	for i := lo; i < hi; i++ {
		base := int(idx[i]) * RecordStride
		for d := 0; d < RecordStride; d++ {
			v := pts[base+d]
			if v < bmin[d] {
				bmin[d] = v
			}
			if v > bmax[d] {
				bmax[d] = v
			}
		}
	}
	return
}

// build constructs the subtree over idx[lo:hi], returns its node index.
func build(lo, hi, depth int) int32 {
	if depth > maxDepth {
		maxDepth = depth
	}
	ni := int32(len(nodes))
	nodes = append(nodes, kdNode{})
	bmin, bmax := computeBox(lo, hi)
	nodes[ni].bmin = bmin
	nodes[ni].bmax = bmax

	n := hi - lo
	if n <= LeafSize {
		nodes[ni].a = uint32(lo)
		nodes[ni].b = uint32(n) | LeafFlag
		return ni
	}

	// widest-spread dimension (lanes 14,15 are constant 0 -> never chosen)
	dim, spread := 0, -1
	for d := 0; d < Dimensions; d++ {
		s := int(bmax[d]) - int(bmin[d])
		if s > spread {
			spread, dim = s, d
		}
	}
	// sliding-midpoint split value
	mid := int16((int(bmin[dim]) + int(bmax[dim])) / 2)

	// in-place partition: idx[lo:m] have dim <= mid, idx[m:hi] have dim > mid
	i := lo
	for j := lo; j < hi; j++ {
		if pts[int(idx[j])*RecordStride+dim] <= mid {
			idx[i], idx[j] = idx[j], idx[i]
			i++
		}
	}
	m := i
	if m == lo || m == hi {
		// degenerate (e.g. many ties): balanced median split so both children
		// are non-empty. Box correctness (not the split value) is what the
		// runtime pruning relies on, so any non-trivial split is exact.
		sub := idx[lo:hi]
		sort.Slice(sub, func(x, y int) bool {
			return pts[int(sub[x])*RecordStride+dim] < pts[int(sub[y])*RecordStride+dim]
		})
		m = lo + n/2
	}

	left := build(lo, m, depth+1)
	right := build(m, hi, depth+1)
	nodes[ni].a = uint32(left)
	nodes[ni].b = uint32(right) // high bit clear (nodeCount < 2^31)
	return ni
}

func main() {
	in, err := os.Open("resources/references.json.gz")
	if err != nil {
		log.Fatalf("open references.json.gz: %v", err)
	}
	defer in.Close()
	gz, err := gzip.NewReader(in)
	if err != nil {
		log.Fatalf("gzip reader: %v", err)
	}
	defer gz.Close()

	dec := json.NewDecoder(gz)
	if _, err := dec.Token(); err != nil { // consume opening '['
		log.Fatalf("expected JSON array open: %v", err)
	}
	N := 0
	pts = make([]int16, 0, 3_000_000*RecordStride)
	fraud = make([]uint8, 0, 3_000_000)
	for dec.More() {
		var row struct {
			Vector [Dimensions]float32 `json:"vector"`
			Label  string              `json:"label"`
		}
		if err := dec.Decode(&row); err != nil {
			log.Fatalf("decode record %d: %v", N, err)
		}
		var vec [RecordStride]int16
		for d := 0; d < Dimensions; d++ {
			vec[d] = quantizeI16(float64(row.Vector[d]))
		}
		// lanes 14,15 stay 0 (AVX2 padding contract)
		pts = append(pts, vec[:]...)
		var f uint8
		if row.Label == "fraud" {
			f = 1
		}
		fraud = append(fraud, f)
		N++
	}
	log.Printf("decoded %d records", N)
	if N == 0 {
		log.Fatal("no records")
	}

	idx = make([]int32, N)
	for i := range idx {
		idx[i] = int32(i)
	}
	nodes = make([]kdNode, 0, 2*N/LeafSize+16)
	build(0, N, 0)
	leaves, maxLeaf, sumLeaf := 0, 0, 0
	for _, nd := range nodes {
		if nd.b&LeafFlag != 0 {
			c := int(nd.b & CountMask)
			leaves++
			sumLeaf += c
			if c > maxLeaf {
				maxLeaf = c
			}
		}
	}
	log.Printf("KD-tree: %d nodes, %d leaves, maxDepth=%d, avgLeaf=%.1f maxLeaf=%d",
		len(nodes), leaves, maxDepth, float64(sumLeaf)/float64(leaves), maxLeaf)

	// reorder data + isFraud into leaf order (idx permutation)
	sortedData := make([]int16, N*RecordStride)
	sortedFraud := make([]uint8, N)
	for p := 0; p < N; p++ {
		src := int(idx[p]) * RecordStride
		copy(sortedData[p*RecordStride:(p+1)*RecordStride], pts[src:src+RecordStride])
		sortedFraud[p] = fraud[idx[p]]
	}

	out, err := os.Create("resources/index.bin")
	if err != nil {
		log.Fatalf("create index.bin: %v", err)
	}
	defer out.Close()

	var header [HeaderSize]byte
	header[0] = FormatVersion
	binary.LittleEndian.PutUint32(header[4:8], uint32(N))
	binary.LittleEndian.PutUint32(header[8:12], uint32(len(nodes)))
	mustWrite(out, header[:])

	// nodes (72 bytes each: 16 i16 bmin, 16 i16 bmax, u32 a, u32 b — all LE)
	nodeBuf := make([]byte, len(nodes)*NodeBytes)
	for i, nd := range nodes {
		o := i * NodeBytes
		for d := 0; d < RecordStride; d++ {
			binary.LittleEndian.PutUint16(nodeBuf[o+d*2:], uint16(nd.bmin[d]))
			binary.LittleEndian.PutUint16(nodeBuf[o+32+d*2:], uint16(nd.bmax[d]))
		}
		binary.LittleEndian.PutUint32(nodeBuf[o+64:o+68], nd.a)
		binary.LittleEndian.PutUint32(nodeBuf[o+68:o+72], nd.b)
	}
	mustWrite(out, nodeBuf)

	// data (N*16 int16, LE)
	dataBuf := make([]byte, N*RecordStride*2)
	for i, v := range sortedData {
		binary.LittleEndian.PutUint16(dataBuf[i*2:], uint16(v))
	}
	mustWrite(out, dataBuf)
	mustWrite(out, sortedFraud)

	expected := int64(HeaderSize) + int64(len(nodes))*NodeBytes +
		int64(N)*int64(RecordStride)*2 + int64(N)
	if stat, err := out.Stat(); err == nil {
		log.Printf("wrote index.bin: %d bytes (%.1f MB, expected %d)",
			stat.Size(), float64(stat.Size())/1e6, expected)
	}
}

func mustWrite(w *os.File, b []byte) {
	if _, err := w.Write(b); err != nil {
		log.Fatal(err)
	}
}

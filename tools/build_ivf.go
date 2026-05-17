//go:build ignore

package main

import (
	"compress/gzip"
	"encoding/binary"
	"encoding/json"
	"flag"
	"log"
	"math"
	"os"
	"runtime"
	"sync"
)

const (
	Dimensions    = 14
	RecordStride  = 16
	FormatVersion = uint8(4)
	HeaderSize    = 16
)

// Binary format v4 (IVF):
//
//	[0]     formatVersion (uint8) = 4
//	[1:4]   reserved
//	[4:8]   centroidCount K (uint32 LE)
//	[8:12]  totalRecords  N (uint32 LE)
//	[12:16] reserved
//	[16 .. 16+K*16]              centroids (each 16 bytes; AVX2 stride)
//	[X .. X + K*4]               clusterOffsets uint32[K] (start record index of each cluster)
//	[X .. X + K*4]               clusterSizes   uint32[K]
//	[X .. X + N*16]              data, sorted by cluster
//	[X .. X + N]                 isFraud, same order as data

func clampQuantize(x float64) uint8 {
	if x < 0.0 {
		return 0
	}
	if x > 1.0 {
		return 255
	}
	return uint8(math.Round(x*254.0)) + 1
}

// distSq computes the squared L2 distance over 16-byte vectors. Unrolled so
// the Go compiler can register-allocate aggressively.
func distSq(a, b []uint8) uint32 {
	_ = a[15] // bounds check hoist
	_ = b[15]
	d0 := int32(a[0]) - int32(b[0])
	d1 := int32(a[1]) - int32(b[1])
	d2 := int32(a[2]) - int32(b[2])
	d3 := int32(a[3]) - int32(b[3])
	d4 := int32(a[4]) - int32(b[4])
	d5 := int32(a[5]) - int32(b[5])
	d6 := int32(a[6]) - int32(b[6])
	d7 := int32(a[7]) - int32(b[7])
	d8 := int32(a[8]) - int32(b[8])
	d9 := int32(a[9]) - int32(b[9])
	d10 := int32(a[10]) - int32(b[10])
	d11 := int32(a[11]) - int32(b[11])
	d12 := int32(a[12]) - int32(b[12])
	d13 := int32(a[13]) - int32(b[13])
	d14 := int32(a[14]) - int32(b[14])
	d15 := int32(a[15]) - int32(b[15])
	return uint32(d0*d0 + d1*d1 + d2*d2 + d3*d3 + d4*d4 + d5*d5 + d6*d6 + d7*d7 +
		d8*d8 + d9*d9 + d10*d10 + d11*d11 + d12*d12 + d13*d13 + d14*d14 + d15*d15)
}

// assignAll fills assign[r] with the index of the nearest centroid for every
// record. Parallel across NumCPU goroutines (embarrassingly parallel).
func assignAll(data, centroids []uint8, K, N int, assign []int32) {
	workers := runtime.NumCPU()
	chunk := (N + workers - 1) / workers
	var wg sync.WaitGroup
	for w := 0; w < workers; w++ {
		start := w * chunk
		end := start + chunk
		if start >= N {
			break
		}
		if end > N {
			end = N
		}
		wg.Add(1)
		go func(start, end int) {
			defer wg.Done()
			for r := start; r < end; r++ {
				rec := data[r*RecordStride : (r+1)*RecordStride]
				var best int32
				var bestDist uint32 = math.MaxUint32
				for c := 0; c < K; c++ {
					cent := centroids[c*RecordStride : (c+1)*RecordStride]
					d := distSq(cent, rec)
					if d < bestDist {
						bestDist = d
						best = int32(c)
					}
				}
				assign[r] = best
			}
		}(start, end)
	}
	wg.Wait()
}

func main() {
	K := flag.Int("k", 2048, "number of centroids")
	iters := flag.Int("iters", 1, "Lloyd iterations after seed (0 = seed only)")
	flag.Parse()

	// --- Phase 1: stream-decode references into flat uint8 buffers ---
	in, err := os.Open("resources/references.json.gz")
	if err != nil {
		log.Fatalf("open references.json.gz: %v", err)
	}
	defer in.Close()
	gz, err := gzip.NewReader(in)
	if err != nil {
		log.Fatalf("gzip: %v", err)
	}
	defer gz.Close()

	dec := json.NewDecoder(gz)
	if _, err := dec.Token(); err != nil {
		log.Fatalf("JSON array open: %v", err)
	}

	data := make([]uint8, 0, 3_000_000*RecordStride)
	isFraud := make([]uint8, 0, 3_000_000)
	for dec.More() {
		var row struct {
			Vector [Dimensions]float32 `json:"vector"`
			Label  string              `json:"label"`
		}
		if err := dec.Decode(&row); err != nil {
			log.Fatalf("decode at record %d: %v", len(isFraud), err)
		}
		for d := 0; d < Dimensions; d++ {
			data = append(data, clampQuantize(float64(row.Vector[d])))
		}
		for d := Dimensions; d < RecordStride; d++ {
			data = append(data, 0)
		}
		if row.Label == "fraud" {
			isFraud = append(isFraud, 1)
		} else {
			isFraud = append(isFraud, 0)
		}
	}
	N := len(isFraud)
	log.Printf("loaded %d records", N)

	// --- Phase 2: pick K seed centroids by evenly-spaced stride ---
	centroids := make([]uint8, *K*RecordStride)
	step := N / *K
	if step < 1 {
		step = 1
	}
	for i := 0; i < *K; i++ {
		src := i * step
		if src >= N {
			src = N - 1
		}
		copy(centroids[i*RecordStride:(i+1)*RecordStride], data[src*RecordStride:(src+1)*RecordStride])
	}
	log.Printf("seeded %d centroids by stride %d", *K, step)

	// --- Phase 3: Lloyd iterations ---
	assign := make([]int32, N)
	prev := make([]int32, N)
	for r := range prev {
		prev[r] = -1
	}
	for iter := 0; iter <= *iters; iter++ {
		log.Printf("iter %d: assigning %d records to %d centroids (parallel %d workers)...", iter, N, *K, runtime.NumCPU())
		assignAll(data, centroids, *K, N, assign)

		changed := 0
		for r := 0; r < N; r++ {
			if assign[r] != prev[r] {
				changed++
			}
		}
		log.Printf("iter %d: %d assignments changed", iter, changed)
		copy(prev, assign)

		if iter == *iters {
			break
		}

		// M-step: recompute centroids as integer means
		sums := make([][RecordStride]uint64, *K)
		counts := make([]uint64, *K)
		for r := 0; r < N; r++ {
			c := assign[r]
			counts[c]++
			rec := data[r*RecordStride : (r+1)*RecordStride]
			for d := 0; d < RecordStride; d++ {
				sums[c][d] += uint64(rec[d])
			}
		}
		empty := 0
		for c := 0; c < *K; c++ {
			if counts[c] == 0 {
				empty++
				continue
			}
			for d := 0; d < RecordStride; d++ {
				centroids[c*RecordStride+d] = uint8(sums[c][d] / counts[c])
			}
		}
		if empty > 0 {
			log.Printf("iter %d: %d empty clusters (kept previous centroid)", iter, empty)
		}
	}

	// --- Phase 4: bucket sort by cluster ---
	clusterSizes := make([]uint32, *K)
	for _, c := range assign {
		clusterSizes[c]++
	}
	clusterOffsets := make([]uint32, *K)
	var acc uint32
	for c := 0; c < *K; c++ {
		clusterOffsets[c] = acc
		acc += clusterSizes[c]
	}

	sortedData := make([]uint8, N*RecordStride)
	sortedIsFraud := make([]uint8, N)
	cursor := make([]uint32, *K)
	copy(cursor, clusterOffsets)
	for r := 0; r < N; r++ {
		c := assign[r]
		dst := int(cursor[c])
		copy(sortedData[dst*RecordStride:(dst+1)*RecordStride], data[r*RecordStride:(r+1)*RecordStride])
		sortedIsFraud[dst] = isFraud[r]
		cursor[c]++
	}

	// --- Phase 5: serialize ---
	out, err := os.Create("resources/index.bin")
	if err != nil {
		log.Fatalf("create index.bin: %v", err)
	}
	defer out.Close()

	var header [HeaderSize]byte
	header[0] = FormatVersion
	binary.LittleEndian.PutUint32(header[4:8], uint32(*K))
	binary.LittleEndian.PutUint32(header[8:12], uint32(N))

	if _, err := out.Write(header[:]); err != nil {
		log.Fatal(err)
	}
	if _, err := out.Write(centroids); err != nil {
		log.Fatal(err)
	}
	if err := binary.Write(out, binary.LittleEndian, clusterOffsets); err != nil {
		log.Fatal(err)
	}
	if err := binary.Write(out, binary.LittleEndian, clusterSizes); err != nil {
		log.Fatal(err)
	}
	if _, err := out.Write(sortedData); err != nil {
		log.Fatal(err)
	}
	if _, err := out.Write(sortedIsFraud); err != nil {
		log.Fatal(err)
	}

	min, max := clusterSizes[0], clusterSizes[0]
	for _, s := range clusterSizes {
		if s < min {
			min = s
		}
		if s > max {
			max = s
		}
	}
	log.Printf("clusters: K=%d, avg=%d, min=%d, max=%d", *K, uint32(N)/uint32(*K), min, max)
	if stat, err := out.Stat(); err == nil {
		log.Printf("wrote index.bin: %d bytes (%.1f MB)", stat.Size(), float64(stat.Size())/1e6)
	}
}

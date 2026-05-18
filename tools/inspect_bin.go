//go:build ignore

package main

import (
	"encoding/binary"
	"flag"
	"fmt"
	"log"
	"os"
	"sort"
)

const (
	Dimensions     = 14
	RecordStride   = 16
	HeaderSize     = 16
	PartitionCount = 4096
)

func main() {
	path := flag.String("path", "resources/index.bin", "path to index.bin")
	flag.Parse()

	raw, err := os.ReadFile(*path)
	if err != nil {
		log.Fatalf("read %s: %v", *path, err)
	}
	if len(raw) < HeaderSize {
		log.Fatalf("file too short: %d bytes", len(raw))
	}

	ver := raw[0]
	if ver != 9 {
		log.Fatalf("unsupported format version %d (expected 9)", ver)
	}
	total := binary.LittleEndian.Uint32(raw[4:8])

	expectedSize := int64(HeaderSize) +
		int64(PartitionCount)*8 + // offsets + sizes
		int64(PartitionCount)*int64(RecordStride)*2 + // min + max bounding boxes
		int64(total)*int64(RecordStride+1)

	fmt.Printf("file:           %s\n", *path)
	fmt.Printf("size:           %d bytes (%.2f MB)\n", len(raw), float64(len(raw))/1e6)
	fmt.Printf("expected size:  %d bytes (matches: %t)\n", expectedSize, int64(len(raw)) == expectedSize)
	fmt.Printf("format version: %d (feature-hash partition)\n", ver)
	fmt.Printf("dimensions:     %d (stride %d)\n", Dimensions, RecordStride)
	fmt.Printf("partitions:     %d\n", PartitionCount)
	fmt.Printf("total:          %d records\n", total)

	if int64(len(raw)) != expectedSize {
		log.Fatalf("size mismatch — abort")
	}

	off := HeaderSize
	offsets := make([]uint32, PartitionCount)
	for i := 0; i < PartitionCount; i++ {
		offsets[i] = binary.LittleEndian.Uint32(raw[off+i*4 : off+i*4+4])
	}
	off += PartitionCount * 4
	sizes := make([]uint32, PartitionCount)
	for i := 0; i < PartitionCount; i++ {
		sizes[i] = binary.LittleEndian.Uint32(raw[off+i*4 : off+i*4+4])
	}
	off += PartitionCount * 4

	minS, maxS, sumS := ^uint32(0), uint32(0), uint32(0)
	empty := 0
	for _, s := range sizes {
		if s < minS {
			minS = s
		}
		if s > maxS {
			maxS = s
		}
		sumS += s
		if s == 0 {
			empty++
		}
	}
	avg := sumS / PartitionCount
	fmt.Printf("partition sizes: avg=%d min=%d max=%d empty=%d (spread max/avg = %.2fx)\n",
		avg, minS, maxS, empty, float64(maxS)/float64(avg))

	type ps struct {
		idx  int
		size uint32
	}
	sorted := make([]ps, 0, PartitionCount)
	for i, s := range sizes {
		sorted = append(sorted, ps{i, s})
	}
	sort.Slice(sorted, func(i, j int) bool { return sorted[i].size > sorted[j].size })
	fmt.Println("top 5 largest:")
	for i := 0; i < 5; i++ {
		fmt.Printf("  partition 0x%03X: %d records\n", sorted[i].idx, sorted[i].size)
	}

	// Skip bounding boxes (min + max blocks) — not displayed but accounted for in offset
	off += PartitionCount * RecordStride * 2

	dataLen := int(total) * RecordStride
	data := raw[off : off+dataLen]
	off += dataLen
	isFraud := raw[off : off+int(total)]

	frauds := 0
	for _, b := range isFraud {
		if b == 1 {
			frauds++
		}
	}
	fmt.Printf("fraud labels:   %d (%.2f%%)\n", frauds, 100.0*float64(frauds)/float64(total))

	// Sanity check: recompute partition_key for a sample and compare
	mismatches := 0
	checked := 0
	for partIdx := 0; partIdx < PartitionCount; partIdx++ {
		start := offsets[partIdx]
		size := sizes[partIdx]
		if size == 0 {
			continue
		}
		// check first record of each partition
		vec := data[start*uint32(RecordStride) : (start+1)*uint32(RecordStride)]
		recomputed := partitionKey(vec)
		checked++
		if int(recomputed) != partIdx {
			mismatches++
			if mismatches <= 3 {
				fmt.Printf("MISMATCH partition 0x%03X first record: recomputed 0x%03X\n", partIdx, recomputed)
			}
		}
	}
	fmt.Printf("integrity check: %d partitions sampled, %d mismatches\n", checked, mismatches)
}

func partitionKey(vec []uint8) uint16 {
	var k uint16 = 0
	if vec[5] == 0 && vec[6] == 0 {
		k |= 1 << 0
	}
	if vec[9] > 128 {
		k |= 1 << 1
	}
	if vec[10] > 128 {
		k |= 1 << 2
	}
	if vec[11] > 128 {
		k |= 1 << 3
	}
	k |= uint16(vec[12]>>6) << 4
	k |= uint16(vec[2]>>6) << 6
	if vec[7] > 128 {
		k |= 1 << 8
	}
	if vec[8] > 128 {
		k |= 1 << 9
	}
	if vec[0] > 128 {
		k |= 1 << 10
	}
	if vec[3] > 128 {
		k |= 1 << 11
	}
	return k
}

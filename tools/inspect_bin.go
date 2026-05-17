//go:build ignore

package main

import (
	"encoding/binary"
	"flag"
	"fmt"
	"log"
	"os"
)

const (
	Dimensions   = 14
	RecordStride = 16
	HeaderSize   = 16
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
	if ver != 4 {
		log.Fatalf("unsupported format version %d (expected 4)", ver)
	}
	K := binary.LittleEndian.Uint32(raw[4:8])
	N := binary.LittleEndian.Uint32(raw[8:12])

	expectedSize := int64(HeaderSize) +
		int64(K)*int64(RecordStride) + // centroids
		int64(K)*4 + // clusterOffsets
		int64(K)*4 + // clusterSizes
		int64(N)*int64(RecordStride) + // data
		int64(N) // isFraud

	fmt.Printf("file:           %s\n", *path)
	fmt.Printf("size:           %d bytes (%.2f MB)\n", len(raw), float64(len(raw))/1e6)
	fmt.Printf("expected size:  %d bytes (matches: %t)\n", expectedSize, int64(len(raw)) == expectedSize)
	fmt.Printf("format version: %d (IVF)\n", ver)
	fmt.Printf("dimensions:     %d (stride %d)\n", Dimensions, RecordStride)
	fmt.Printf("centroids (K):  %d\n", K)
	fmt.Printf("records (N):    %d\n", N)

	if int64(len(raw)) != expectedSize {
		log.Fatalf("size mismatch — abort")
	}

	off := int64(HeaderSize)
	off += int64(K) * int64(RecordStride)
	coBytes := raw[off : off+int64(K)*4]
	off += int64(K) * 4
	csBytes := raw[off : off+int64(K)*4]
	off += int64(K) * 4
	// data section follows but we only need cluster sizes for stats
	_ = off
	_ = coBytes

	// cluster size histogram
	var minSize, maxSize, sumSize uint32 = ^uint32(0), 0, 0
	zeros := 0
	for c := uint32(0); c < K; c++ {
		s := binary.LittleEndian.Uint32(csBytes[c*4 : c*4+4])
		if s < minSize {
			minSize = s
		}
		if s > maxSize {
			maxSize = s
		}
		sumSize += s
		if s == 0 {
			zeros++
		}
	}
	fmt.Printf("cluster sizes:  avg=%d min=%d max=%d empty=%d\n",
		sumSize/K, minSize, maxSize, zeros)

	dataOff := int64(HeaderSize) + int64(K)*int64(RecordStride) + int64(K)*8
	isFraudOff := dataOff + int64(N)*int64(RecordStride)
	isFraudSection := raw[isFraudOff : isFraudOff+int64(N)]
	frauds := 0
	for _, b := range isFraudSection {
		if b == 1 {
			frauds++
		}
	}
	fmt.Printf("fraud labels:   %d (%.2f%%)\n", frauds, 100.0*float64(frauds)/float64(N))
}

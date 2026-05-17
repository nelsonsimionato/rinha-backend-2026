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
	if ver != 5 {
		log.Fatalf("unsupported format version %d (expected 5)", ver)
	}
	totalWH := binary.LittleEndian.Uint32(raw[4:8])
	totalNH := binary.LittleEndian.Uint32(raw[8:12])
	total := totalWH + totalNH

	expectedSize := int64(HeaderSize) +
		int64(totalWH)*int64(RecordStride+1) +
		int64(totalNH)*int64(RecordStride+1)

	fmt.Printf("file:           %s\n", *path)
	fmt.Printf("size:           %d bytes (%.2f MB)\n", len(raw), float64(len(raw))/1e6)
	fmt.Printf("expected size:  %d bytes (matches: %t)\n", expectedSize, int64(len(raw)) == expectedSize)
	fmt.Printf("format version: %d (brute-force partition)\n", ver)
	fmt.Printf("dimensions:     %d (stride %d)\n", Dimensions, RecordStride)
	fmt.Printf("with_history:   %d (%.2f%%)\n", totalWH, 100.0*float64(totalWH)/float64(total))
	fmt.Printf("no_history:     %d (%.2f%%)\n", totalNH, 100.0*float64(totalNH)/float64(total))
	fmt.Printf("total:          %d\n", total)

	if int64(len(raw)) != expectedSize {
		log.Fatalf("size mismatch — abort")
	}

	off := HeaderSize
	dataWH := raw[off : off+int(totalWH)*RecordStride]
	off += int(totalWH) * RecordStride
	isFraudWH := raw[off : off+int(totalWH)]
	off += int(totalWH)
	dataNH := raw[off : off+int(totalNH)*RecordStride]
	off += int(totalNH) * RecordStride
	isFraudNH := raw[off : off+int(totalNH)]

	fraudWH, fraudNH := 0, 0
	for _, b := range isFraudWH {
		if b == 1 {
			fraudWH++
		}
	}
	for _, b := range isFraudNH {
		if b == 1 {
			fraudNH++
		}
	}
	totalFraud := fraudWH + fraudNH
	fmt.Printf("fraud labels:   %d (%.2f%% overall)\n", totalFraud, 100.0*float64(totalFraud)/float64(total))
	fmt.Printf("  in WH:        %d (%.2f%% of WH)\n", fraudWH, 100.0*float64(fraudWH)/float64(totalWH))
	fmt.Printf("  in NH:        %d (%.2f%% of NH)\n", fraudNH, 100.0*float64(fraudNH)/float64(totalNH))

	whSentinels := 0
	for i := 0; i < int(totalWH); i++ {
		off := i * RecordStride
		if dataWH[off+5] == 0 || dataWH[off+6] == 0 {
			whSentinels++
		}
	}
	nhNonSentinels := 0
	for i := 0; i < int(totalNH); i++ {
		off := i * RecordStride
		if dataNH[off+5] != 0 || dataNH[off+6] != 0 {
			nhNonSentinels++
		}
	}
	fmt.Printf("partition integrity:\n")
	fmt.Printf("  WH with byte 0 at idx 5/6: %d (should be 0)\n", whSentinels)
	fmt.Printf("  NH with non-0 at idx 5/6:  %d (should be 0)\n", nhNonSentinels)
}

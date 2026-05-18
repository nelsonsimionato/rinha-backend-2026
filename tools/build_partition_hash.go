//go:build ignore

package main

import (
	"compress/gzip"
	"encoding/binary"
	"encoding/json"
	"log"
	"math"
	"os"
)

const (
	Dimensions     = 14
	RecordStride   = 16
	FormatVersion  = uint8(11)
	HeaderSize     = 16
	PartitionCount = 16384
)

// Binary format v7 (feature-hash partition, 1024 buckets):
//
//	[0]      formatVersion (uint8) = 7
//	[1:4]    reserved
//	[4:8]    totalRecords N (uint32 LE)
//	[8:16]   reserved
//	[16 .. 16+1024*4]     partitionOffsets uint32[1024] (start record idx)
//	[X .. X+1024*4]       partitionSizes   uint32[1024]
//	[X .. X+N*16]         data uint8[N*16], sorted by partition
//	[X .. X+N]            isFraud uint8[N], same order
//
// partitionKey packs 10 bits from the most fraud-discriminative dimensions:
//   bit 0: history sentinel (1 = no_history, 0 = has_history)
//   bit 1: is_online            (dim 9)
//   bit 2: card_present         (dim 10)
//   bit 3: unknown_merchant     (dim 11)
//   bits 4-5: mcc_risk          (dim 12) bucketed to 4 levels
//   bits 6-7: amount_vs_avg     (dim 2)  bucketed to 4 levels
//   bit 8: km_from_home > 0.5   (dim 7)  far-from-home is a strong fraud signal
//   bit 9: tx_count_24h > 0.5   (dim 8)  >10 tx in 24h is a strong fraud signal

func clampQuantize(x float64) uint8 {
	if x < 0.0 {
		return 0
	}
	if x > 1.0 {
		return 255
	}
	return uint8(math.Round(x*254.0)) + 1
}

// partitionKey computes the partition for a quantized 16-byte vector.
// Must match runtime's partitionKey in main.go byte-for-byte.
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
	// bit 10: amount > 0.5 of max_amount (dim 0, byte > 128) — high spending splits legit clusters
	if vec[0] > 128 {
		k |= 1 << 10
	}
	// bit 11: hour_of_day > 12 (dim 3, byte > 128) — afternoon/night vs morning
	if vec[3] > 128 {
		k |= 1 << 11
	}
	// bit 12: day_of_week > 3 (dim 4, byte > 128) — Thu/Fri/Sat/Sun vs Mon/Tue/Wed
	if vec[4] > 128 {
		k |= 1 << 12
	}
	// bit 13: merchant_avg_amount > 0.5 (dim 13, byte > 128) — merchant tier (premium vs everyday)
	// Splits ~50/50 in real data → halves the largest partitions
	if vec[13] > 128 {
		k |= 1 << 13
	}
	return k
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
	if _, err := dec.Token(); err != nil {
		log.Fatalf("expected JSON array open: %v", err)
	}

	// First pass: read all records, quantize, compute partition_key, accumulate sizes
	type record struct {
		vec   [RecordStride]uint8
		fraud uint8
		key   uint16
	}
	records := make([]record, 0, 3_000_000)
	for dec.More() {
		var row struct {
			Vector [Dimensions]float32 `json:"vector"`
			Label  string              `json:"label"`
		}
		if err := dec.Decode(&row); err != nil {
			log.Fatalf("decode record %d: %v", len(records), err)
		}
		var r record
		for d := 0; d < Dimensions; d++ {
			r.vec[d] = clampQuantize(float64(row.Vector[d]))
		}
		// bytes 14, 15 are zero pad for AVX2 — already zeroed
		r.key = partitionKey(r.vec[:])
		if row.Label == "fraud" {
			r.fraud = 1
		}
		records = append(records, r)
	}
	N := uint32(len(records))
	log.Printf("decoded %d records", N)

	// Tally partition sizes
	var sizes [PartitionCount]uint32
	for _, r := range records {
		sizes[r.key]++
	}
	// Compute offsets (prefix sum)
	var offsets [PartitionCount]uint32
	var acc uint32
	for i := 0; i < PartitionCount; i++ {
		offsets[i] = acc
		acc += sizes[i]
	}

	// Histogram of partition sizes — useful for diagnosis
	var minS, maxS, sumS, emptyP uint32
	minS = ^uint32(0)
	for _, s := range sizes {
		if s < minS {
			minS = s
		}
		if s > maxS {
			maxS = s
		}
		sumS += s
		if s == 0 {
			emptyP++
		}
	}
	log.Printf("partitions: count=%d, avg=%d, min=%d, max=%d, empty=%d",
		PartitionCount, sumS/PartitionCount, minS, maxS, emptyP)

	// Bucket sort into output buffers
	sortedData := make([]uint8, int(N)*RecordStride)
	sortedFraud := make([]uint8, N)
	cursor := offsets // copy
	for _, r := range records {
		dst := cursor[r.key]
		copy(sortedData[int(dst)*RecordStride:int(dst+1)*RecordStride], r.vec[:])
		sortedFraud[dst] = r.fraud
		cursor[r.key]++
	}

	// Axis-aligned bounding box (min, max) per partition for bound-pruning at query time.
	partitionMin := make([]uint8, PartitionCount*RecordStride)
	partitionMax := make([]uint8, PartitionCount*RecordStride)
	for p := uint32(0); p < PartitionCount; p++ {
		size := sizes[p]
		base := p * uint32(RecordStride)
		if size == 0 {
			// Empty: min=255 max=0 → any lower-bound computes to a huge number → skipped at runtime
			for d := 0; d < RecordStride; d++ {
				partitionMin[base+uint32(d)] = 255
				partitionMax[base+uint32(d)] = 0
			}
			continue
		}
		start := offsets[p]
		for d := 0; d < RecordStride; d++ {
			v := sortedData[start*uint32(RecordStride)+uint32(d)]
			partitionMin[base+uint32(d)] = v
			partitionMax[base+uint32(d)] = v
		}
		for r := uint32(1); r < size; r++ {
			recBase := (start + r) * uint32(RecordStride)
			for d := 0; d < RecordStride; d++ {
				v := sortedData[recBase+uint32(d)]
				if v < partitionMin[base+uint32(d)] {
					partitionMin[base+uint32(d)] = v
				}
				if v > partitionMax[base+uint32(d)] {
					partitionMax[base+uint32(d)] = v
				}
			}
		}
	}
	log.Printf("computed bounding boxes for %d partitions (%d KB)",
		PartitionCount, PartitionCount*RecordStride*2/1024)

	out, err := os.Create("resources/index.bin")
	if err != nil {
		log.Fatalf("create index.bin: %v", err)
	}
	defer out.Close()

	var header [HeaderSize]byte
	header[0] = FormatVersion
	binary.LittleEndian.PutUint32(header[4:8], N)
	if _, err := out.Write(header[:]); err != nil {
		log.Fatal(err)
	}
	if err := binary.Write(out, binary.LittleEndian, offsets[:]); err != nil {
		log.Fatal(err)
	}
	if err := binary.Write(out, binary.LittleEndian, sizes[:]); err != nil {
		log.Fatal(err)
	}
	if _, err := out.Write(partitionMin); err != nil {
		log.Fatal(err)
	}
	if _, err := out.Write(partitionMax); err != nil {
		log.Fatal(err)
	}
	if _, err := out.Write(sortedData); err != nil {
		log.Fatal(err)
	}
	if _, err := out.Write(sortedFraud); err != nil {
		log.Fatal(err)
	}

	expected := int64(HeaderSize) +
		int64(PartitionCount)*8 + // offsets + sizes
		int64(PartitionCount)*int64(RecordStride)*2 + // min + max boxes
		int64(N)*int64(RecordStride+1)
	if stat, err := out.Stat(); err == nil {
		log.Printf("wrote index.bin: %d bytes (%.1f MB, expected %d)",
			stat.Size(), float64(stat.Size())/1e6, expected)
	}
}

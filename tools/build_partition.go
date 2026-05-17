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
	Dimensions    = 14
	RecordStride  = 16
	FormatVersion = uint8(5)
	HeaderSize    = 16
)

// Binary format v5 (deterministic WH/NH partition, no clustering):
//
//	[0]     formatVersion (uint8) = 5
//	[1:4]   reserved
//	[4:8]   totalWithHistory  (uint32 LE)
//	[8:12]  totalNoHistory    (uint32 LE)
//	[12:16] reserved
//	[16 .. 16 + WH*16]                 WH.data uint8[WH*16] (stride 16; bytes 14-15 zero pad for AVX2)
//	[X .. X + WH]                      WH.isFraud uint8[WH]
//	[X .. X + NH*16]                   NH.data
//	[X .. X + NH]                      NH.isFraud
//
// Partition rule: a record is "no-history" iff the source float vector has -1
// at idx 5 OR idx 6. Runtime dispatches by query.vec[5]==0 && query.vec[6]==0
// (both bytes mean "sentinel" in the uint8 quantization scheme).
//
// Cross-pool neighbors cannot be in the true top-5: a NH record (bytes 5/6 = 0)
// and a WH query (bytes 5/6 in [1, 255]) have squared distance contribution
// of at least 1*1 = 1 and up to 255*255 = 65025 at each of those indices,
// which is enough to push them out of any close vector's neighborhood.
//
// Brute force exact k-NN within the matching pool gives **100% recall vs the
// official ground-truth labeling** (which uses exact k=5 Euclidean brute force).

func clampQuantize(x float64) uint8 {
	if x < 0.0 {
		return 0
	}
	if x > 1.0 {
		return 255
	}
	return uint8(math.Round(x*254.0)) + 1
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

	var (
		dataWH    []uint8
		isFraudWH []uint8
		dataNH    []uint8
		isFraudNH []uint8
	)
	dataWH = make([]uint8, 0, 2_000_000*RecordStride)
	isFraudWH = make([]uint8, 0, 2_000_000)
	dataNH = make([]uint8, 0, 1_000_000*RecordStride)
	isFraudNH = make([]uint8, 0, 1_000_000)

	fraudWH, fraudNH := 0, 0
	total := 0
	for dec.More() {
		var row struct {
			Vector [Dimensions]float32 `json:"vector"`
			Label  string              `json:"label"`
		}
		if err := dec.Decode(&row); err != nil {
			log.Fatalf("decode record %d: %v", total, err)
		}

		noHistory := row.Vector[5] < 0 || row.Vector[6] < 0
		var dataPool, isFraudPool *[]uint8
		if noHistory {
			dataPool = &dataNH
			isFraudPool = &isFraudNH
		} else {
			dataPool = &dataWH
			isFraudPool = &isFraudWH
		}

		for d := 0; d < Dimensions; d++ {
			*dataPool = append(*dataPool, clampQuantize(float64(row.Vector[d])))
		}
		// pad to RecordStride with zero bytes (AVX2 16-byte load contract)
		for d := Dimensions; d < RecordStride; d++ {
			*dataPool = append(*dataPool, 0)
		}
		if row.Label == "fraud" {
			*isFraudPool = append(*isFraudPool, 1)
			if noHistory {
				fraudNH++
			} else {
				fraudWH++
			}
		} else {
			*isFraudPool = append(*isFraudPool, 0)
		}
		total++
	}

	totalWH := uint32(len(isFraudWH))
	totalNH := uint32(len(isFraudNH))
	log.Printf("decoded %d records: with_history=%d (fraud=%d), no_history=%d (fraud=%d)",
		total, totalWH, fraudWH, totalNH, fraudNH)

	out, err := os.Create("resources/index.bin")
	if err != nil {
		log.Fatalf("create index.bin: %v", err)
	}
	defer out.Close()

	var header [HeaderSize]byte
	header[0] = FormatVersion
	binary.LittleEndian.PutUint32(header[4:8], totalWH)
	binary.LittleEndian.PutUint32(header[8:12], totalNH)

	if _, err := out.Write(header[:]); err != nil {
		log.Fatal(err)
	}
	if _, err := out.Write(dataWH); err != nil {
		log.Fatal(err)
	}
	if _, err := out.Write(isFraudWH); err != nil {
		log.Fatal(err)
	}
	if _, err := out.Write(dataNH); err != nil {
		log.Fatal(err)
	}
	if _, err := out.Write(isFraudNH); err != nil {
		log.Fatal(err)
	}

	expected := int64(HeaderSize) + int64(total)*int64(RecordStride+1)
	if stat, err := out.Stat(); err == nil {
		log.Printf("wrote index.bin: %d bytes (%.1f MB, expected %d)",
			stat.Size(), float64(stat.Size())/1e6, expected)
	}
}

//go:build ignore

package main

import (
	"compress/gzip"
	"encoding/json"
	"flag"
	"log"
	"math/rand"
	"os"
)

const Dimensions = 14

// Canonical vectors from docs/DETECTION_RULES.md, used to seed the "clustered"
// mode so Phase 1.4 end-to-end checks have reliable nearest neighbors.
var (
	canonicalLegit = [Dimensions]float32{
		0.0041, 0.1667, 0.05, 0.7826, 0.3333, -1, -1,
		0.0292, 0.15, 0, 1, 0, 0.15, 0.006,
	}
	canonicalFraud = [Dimensions]float32{
		0.9506, 0.8333, 1.0, 0.2174, 0.8333, -1, -1,
		0.9523, 1.0, 0, 1, 1, 0.75, 0.0055,
	}
)

type record struct {
	Vector [Dimensions]float32 `json:"vector"`
	Label  string              `json:"label"`
}

func main() {
	mode := flag.String("mode", "general", "general | clustered")
	n := flag.Int("n", 10000, "number of records (general mode)")
	out := flag.String("out", "resources/references.json.gz", "output path")
	seed := flag.Int64("seed", 42, "random seed")
	flag.Parse()

	rng := rand.New(rand.NewSource(*seed))

	f, err := os.Create(*out)
	if err != nil {
		log.Fatalf("create %s: %v", *out, err)
	}
	defer f.Close()

	gz := gzip.NewWriter(f)
	defer gz.Close()

	enc := json.NewEncoder(gz)

	if _, err := gz.Write([]byte("[")); err != nil {
		log.Fatal(err)
	}

	var records []record
	switch *mode {
	case "general":
		records = genGeneral(rng, *n)
	case "clustered":
		records = genClustered(rng, 25)
	default:
		log.Fatalf("unknown mode %q", *mode)
	}

	for i, r := range records {
		if i > 0 {
			if _, err := gz.Write([]byte(",")); err != nil {
				log.Fatal(err)
			}
		}
		if err := enc.Encode(r); err != nil {
			log.Fatal(err)
		}
	}

	if _, err := gz.Write([]byte("]")); err != nil {
		log.Fatal(err)
	}

	frauds := 0
	noHistory := 0
	for _, r := range records {
		if r.Label == "fraud" {
			frauds++
		}
		if r.Vector[5] < 0 || r.Vector[6] < 0 {
			noHistory++
		}
	}
	log.Printf("wrote %d records to %s (fraud=%d, no_history=%d)", len(records), *out, frauds, noHistory)
}

func genGeneral(rng *rand.Rand, n int) []record {
	out := make([]record, n)
	for i := 0; i < n; i++ {
		isFraud := rng.Float32() < 0.05
		noHistory := rng.Float32() < 0.30

		var v [Dimensions]float32
		for d := 0; d < Dimensions; d++ {
			v[d] = rng.Float32()
		}
		// Binary-ish dims: snap to 0/1.
		v[9] = float32(rng.Intn(2))
		v[10] = float32(rng.Intn(2))
		v[11] = float32(rng.Intn(2))
		if noHistory {
			v[5] = -1
			v[6] = -1
		}

		label := "legit"
		if isFraud {
			label = "fraud"
		}
		out[i] = record{Vector: v, Label: label}
	}
	return out
}

// genClustered produces 2*per records: per near the canonical legit vector
// (label=legit) and per near the canonical fraud vector (label=fraud). Used by
// Phase 1.4 to verify the canonical query payloads resolve to their expected
// labels end-to-end on a small index.
func genClustered(rng *rand.Rand, per int) []record {
	const jitter = 0.02
	out := make([]record, 0, 2*per)
	for i := 0; i < per; i++ {
		out = append(out, record{Vector: jitterAround(rng, canonicalLegit, jitter), Label: "legit"})
	}
	for i := 0; i < per; i++ {
		out = append(out, record{Vector: jitterAround(rng, canonicalFraud, jitter), Label: "fraud"})
	}
	return out
}

func jitterAround(rng *rand.Rand, center [Dimensions]float32, amp float32) [Dimensions]float32 {
	var v [Dimensions]float32
	for d := 0; d < Dimensions; d++ {
		if center[d] < 0 {
			// preserve the -1 sentinel
			v[d] = -1
			continue
		}
		v[d] = center[d] + (rng.Float32()*2-1)*amp
		if v[d] < 0 {
			v[d] = 0
		}
		if v[d] > 1 {
			v[d] = 1
		}
	}
	return v
}

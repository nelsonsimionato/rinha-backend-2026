//go:build ignore

package main

import (
	"encoding/binary"
	"flag"
	"fmt"
	"log"
	"math"
	"math/rand"
	"os"
	"sort"
	"time"
)

const (
	Dimensions   = 14
	RecordStride = 16
	HeaderSize   = 16
	K            = 5
)

// distSq is the pure-Go reference; intentionally matches distance_amd64.s.
func distSq(a, b []uint8) uint32 {
	_ = a[15]
	_ = b[15]
	var sum uint32
	for i := 0; i < 16; i++ {
		d := int32(a[i]) - int32(b[i])
		sum += uint32(d * d)
	}
	return sum
}

type pair struct {
	dist uint32
	idx  int32
}

// exactKNN: brute-force top-5 over the entire data section.
func exactKNN(query []uint8, data []uint8, n int) [K]int32 {
	type pairLocal pair
	heap := make([]pairLocal, 0, K)
	for r := 0; r < n; r++ {
		d := distSq(query, data[r*RecordStride:(r+1)*RecordStride])
		if len(heap) < K {
			heap = append(heap, pairLocal{d, int32(r)})
			sort.Slice(heap, func(i, j int) bool { return heap[i].dist < heap[j].dist })
		} else if d < heap[K-1].dist {
			heap[K-1] = pairLocal{d, int32(r)}
			sort.Slice(heap, func(i, j int) bool { return heap[i].dist < heap[j].dist })
		}
	}
	var out [K]int32
	for i := 0; i < K; i++ {
		out[i] = heap[i].idx
	}
	return out
}

// ivfKNN: simulate the runtime IVF search.
func ivfKNN(query []uint8, centroids, data []uint8, clusterOffsets, clusterSizes []uint32, KCentroids, probe int) [K]int32 {
	// pick top-probe centroids
	type pairLocal pair
	cents := make([]pairLocal, 0, probe)
	for c := 0; c < KCentroids; c++ {
		d := distSq(query, centroids[c*RecordStride:(c+1)*RecordStride])
		if len(cents) < probe {
			cents = append(cents, pairLocal{d, int32(c)})
			sort.Slice(cents, func(i, j int) bool { return cents[i].dist < cents[j].dist })
		} else if d < cents[probe-1].dist {
			cents[probe-1] = pairLocal{d, int32(c)}
			sort.Slice(cents, func(i, j int) bool { return cents[i].dist < cents[j].dist })
		}
	}
	// scan records in those clusters
	heap := make([]pairLocal, 0, K)
	for _, cp := range cents {
		c := cp.idx
		start := clusterOffsets[c]
		size := clusterSizes[c]
		for r := uint32(0); r < size; r++ {
			recIdx := start + r
			d := distSq(query, data[recIdx*RecordStride:(recIdx+1)*RecordStride])
			if len(heap) < K {
				heap = append(heap, pairLocal{d, int32(recIdx)})
				sort.Slice(heap, func(i, j int) bool { return heap[i].dist < heap[j].dist })
			} else if d < heap[K-1].dist {
				heap[K-1] = pairLocal{d, int32(recIdx)}
				sort.Slice(heap, func(i, j int) bool { return heap[i].dist < heap[j].dist })
			}
		}
	}
	var out [K]int32
	for i := 0; i < K && i < len(heap); i++ {
		out[i] = heap[i].idx
	}
	return out
}

func main() {
	probe := flag.Int("probe", 8, "IVF probe count")
	samples := flag.Int("samples", 200, "number of queries to evaluate (capped by brute-force cost)")
	flag.Parse()

	raw, err := os.ReadFile("resources/index.bin")
	if err != nil {
		log.Fatalf("read index.bin: %v", err)
	}
	if raw[0] != 4 {
		log.Fatalf("expected format v4, got %d", raw[0])
	}
	KCentroids := int(binary.LittleEndian.Uint32(raw[4:8]))
	N := int(binary.LittleEndian.Uint32(raw[8:12]))

	off := HeaderSize
	centroids := raw[off : off+KCentroids*RecordStride]
	off += KCentroids * RecordStride

	clusterOffsets := make([]uint32, KCentroids)
	for c := 0; c < KCentroids; c++ {
		clusterOffsets[c] = binary.LittleEndian.Uint32(raw[off+c*4 : off+c*4+4])
	}
	off += KCentroids * 4

	clusterSizes := make([]uint32, KCentroids)
	for c := 0; c < KCentroids; c++ {
		clusterSizes[c] = binary.LittleEndian.Uint32(raw[off+c*4 : off+c*4+4])
	}
	off += KCentroids * 4

	data := raw[off : off+N*RecordStride]

	fmt.Printf("Index: K=%d centroids, N=%d records, probe=%d\n", KCentroids, N, *probe)
	fmt.Printf("Evaluating recall@%d on %d random queries (brute force vs IVF)...\n", K, *samples)

	rng := rand.New(rand.NewSource(12345))
	totalOverlap := 0
	var bruteTime, ivfTime time.Duration
	for s := 0; s < *samples; s++ {
		// pick a random record to use as query
		qIdx := rng.Intn(N)
		query := data[qIdx*RecordStride : (qIdx+1)*RecordStride]

		t0 := time.Now()
		exact := exactKNN(query, data, N)
		bruteTime += time.Since(t0)

		t1 := time.Now()
		approx := ivfKNN(query, centroids, data, clusterOffsets, clusterSizes, KCentroids, *probe)
		ivfTime += time.Since(t1)

		// count overlap (excluding the query itself if present, which has dist 0)
		exactSet := map[int32]struct{}{}
		for _, idx := range exact {
			exactSet[idx] = struct{}{}
		}
		overlap := 0
		for _, idx := range approx {
			if _, ok := exactSet[idx]; ok {
				overlap++
			}
		}
		totalOverlap += overlap

		if s > 0 && s%50 == 0 {
			log.Printf("  %d / %d", s, *samples)
		}
	}

	maxOverlap := *samples * K
	recall := float64(totalOverlap) / float64(maxOverlap)
	fmt.Printf("\nRecall@%d = %.2f%% (overlap %d / %d)\n", K, 100*recall, totalOverlap, maxOverlap)
	fmt.Printf("brute force avg: %v per query\n", bruteTime/time.Duration(*samples))
	fmt.Printf("IVF avg:         %v per query  (%.0fx speedup)\n",
		ivfTime/time.Duration(*samples), float64(bruteTime)/float64(ivfTime))
	_ = math.Sqrt // keep import
}

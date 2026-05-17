package main

import (
	"math/rand"
	"testing"
)

// referenceDistance is a slow, obviously-correct reference implementation that
// the production distanceSq must match byte-for-byte. Bytes 14 and 15 are
// padding and must be zero in both inputs.
func referenceDistance(q, r *[16]uint8) uint32 {
	var sum uint32
	for i := 0; i < 16; i++ {
		d := int32(q[i]) - int32(r[i])
		sum += uint32(d * d)
	}
	return sum
}

func TestDistanceSqMatchesReference(t *testing.T) {
	rng := rand.New(rand.NewSource(42))

	cases := 1000
	for c := 0; c < cases; c++ {
		var q, r [16]uint8
		for i := 0; i < 14; i++ {
			q[i] = uint8(rng.Intn(256))
			r[i] = uint8(rng.Intn(256))
		}
		// bytes 14, 15 stay 0 (padding)

		want := referenceDistance(&q, &r)
		got := distanceSq(&q[0], &r[0])
		if got != want {
			t.Fatalf("case %d: distanceSq mismatch\n  q=%v\n  r=%v\n  got=%d  want=%d", c, q, r, got, want)
		}
	}
}

func TestDistanceSqEdgeCases(t *testing.T) {
	cases := []struct {
		name    string
		q, r    [16]uint8
		want    uint32
	}{
		{"all zeros", [16]uint8{}, [16]uint8{}, 0},
		{"identical max", [16]uint8{255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 0, 0}, [16]uint8{255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 0, 0}, 0},
		{"max diff at idx 0", [16]uint8{255}, [16]uint8{0}, 255 * 255},
		{"max diff at idx 13", [16]uint8{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 255, 0, 0}, [16]uint8{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, 255 * 255},
		{"diff 1 across all 14 dims", [16]uint8{1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0}, [16]uint8{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, 14},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			got := distanceSq(&tc.q[0], &tc.r[0])
			if got != tc.want {
				t.Fatalf("got %d, want %d", got, tc.want)
			}
		})
	}
}

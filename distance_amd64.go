//go:build amd64

package main

// distanceSqAvx returns the squared Euclidean distance between two 16-byte
// padded uint8 vectors. Implemented in distance_amd64.s.
//
// IMPORTANT: bytes 14 and 15 of both query and ref MUST be 0 — they participate
// in the sum and would distort the distance otherwise.
//
//go:noescape
func distanceSqAvx(query, ref *byte) uint32

// distanceSq is the production entry point. On amd64 it calls the AVX2 path;
// distance_other.go provides the pure-Go fallback for non-amd64.
func distanceSq(query, ref *byte) uint32 {
	return distanceSqAvx(query, ref)
}

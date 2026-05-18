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

// boundDistSqAvx returns the squared lower-bound Euclidean distance from query
// to a partition's axis-aligned bounding box (partMin/partMax). Used to prune
// partitions that cannot possibly contain a closer neighbor than the current
// top-K worst.
//
//go:noescape
func boundDistSqAvx(query, partMin, partMax *byte) uint32

// distanceSq is the production entry point. On amd64 it calls the AVX2 path;
// distance_other.go provides the pure-Go fallback for non-amd64.
func distanceSq(query, ref *byte) uint32 {
	return distanceSqAvx(query, ref)
}

// boundDistSq is the production entry point for partition lower-bound distance.
func boundDistSq(query, partMin, partMax *byte) uint32 {
	return boundDistSqAvx(query, partMin, partMax)
}

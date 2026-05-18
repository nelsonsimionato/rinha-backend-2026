//go:build !amd64

package main

import "unsafe"

// distanceSq is the non-amd64 fallback for the squared Euclidean distance over
// 16-byte padded uint8 vectors. Matches distance_amd64.s byte-for-byte.
func distanceSq(query, ref *byte) uint32 {
	q := (*[16]uint8)(unsafe.Pointer(query))
	r := (*[16]uint8)(unsafe.Pointer(ref))
	var sum uint32
	for i := 0; i < 16; i++ {
		d := int32(q[i]) - int32(r[i])
		sum += uint32(d * d)
	}
	return sum
}

// boundDistSq is the non-amd64 fallback for partition lower-bound distance.
func boundDistSq(query, partMin, partMax *byte) uint32 {
	q := (*[16]uint8)(unsafe.Pointer(query))
	mn := (*[16]uint8)(unsafe.Pointer(partMin))
	mx := (*[16]uint8)(unsafe.Pointer(partMax))
	var sum uint32
	for i := 0; i < 16; i++ {
		qi := int32(q[i])
		mni := int32(mn[i])
		mxi := int32(mx[i])
		var d int32
		switch {
		case qi < mni:
			d = mni - qi
		case qi > mxi:
			d = qi - mxi
		}
		sum += uint32(d * d)
	}
	return sum
}

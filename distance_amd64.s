#include "textflag.h"

// func distanceSqAvx(query, ref *byte) uint32
//
// Squared Euclidean distance between two 16-byte uint8 vectors.
// Bytes 14 and 15 of both query and ref must be 0 (padding).
TEXT ·distanceSqAvx(SB), NOSPLIT, $0-20
	MOVQ query+0(FP), AX
	MOVQ ref+8(FP), BX

	VMOVDQU (AX), X0
	VMOVDQU (BX), X1

	VPMINUB X1, X0, X2
	VPMAXUB X1, X0, X3
	VPSUBB  X2, X3, X4

	VPMOVZXBW X4, Y5
	VPMADDWD Y5, Y5, Y6

	VEXTRACTI128 $1, Y6, X7
	VPADDD X6, X7, X8
	VPSHUFD $0x4E, X8, X9
	VPADDD X8, X9, X8
	VPSHUFD $0xB1, X8, X9
	VPADDD X8, X9, X8
	VMOVD X8, AX

	VZEROUPPER

	MOVL AX, ret+16(FP)
	RET

// func boundDistSqAvx(query, partMin, partMax *byte) uint32
//
// Squared lower-bound distance from query to a partition's axis-aligned
// bounding box. For each dim:
//   if query[d] < partMin[d]: contribution = (partMin[d] - query[d])²
//   if query[d] > partMax[d]: contribution = (query[d] - partMax[d])²
//   else:                      contribution = 0
//
// Uses VPSUBUSB (unsigned saturating subtract) to compute both
// "max(0, partMin - query)" and "max(0, query - partMax)" in parallel,
// then OR via VPMAXUB (only one can be non-zero per byte).
TEXT ·boundDistSqAvx(SB), NOSPLIT, $0-28
	MOVQ query+0(FP), AX
	MOVQ partMin+8(FP), BX
	MOVQ partMax+16(FP), CX

	VMOVDQU (AX), X0    // query
	VMOVDQU (BX), X1    // partMin
	VMOVDQU (CX), X2    // partMax

	// X3 = max(0, partMin - query): non-zero where query < partMin
	VPSUBUSB X0, X1, X3
	// X4 = max(0, query - partMax): non-zero where query > partMax
	VPSUBUSB X2, X0, X4
	// X5 = per-byte edge distance (only one of X3[d], X4[d] is non-zero)
	VPMAXUB X3, X4, X5

	// Square: zero-extend to words, pmaddwd, horizontal sum
	VPMOVZXBW X5, Y6
	VPMADDWD Y6, Y6, Y7

	VEXTRACTI128 $1, Y7, X8
	VPADDD X7, X8, X7
	VPSHUFD $0x4E, X7, X8
	VPADDD X7, X8, X7
	VPSHUFD $0xB1, X7, X8
	VPADDD X7, X8, X7
	VMOVD X7, AX

	VZEROUPPER

	MOVL AX, ret+24(FP)
	RET

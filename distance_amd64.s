#include "textflag.h"

// func distanceSqAvx(query, ref *byte) uint32
//
// Computes the squared Euclidean distance between two 16-byte uint8 vectors.
// Bytes 14 and 15 of both query and ref must be 0 (padding) so they contribute
// nothing to the sum.
//
// Algorithm:
//   |q - r| = max(q,r) - min(q,r)             (unsigned abs diff)
//   square each byte by zero-extending to words and pmaddwd
//   horizontal-sum 8 dwords -> 1 uint32
//
// On Haswell: ~10 cycles per call, dominated by vpmaddwd + horizontal sum.
TEXT ·distanceSqAvx(SB), NOSPLIT, $0-20
	MOVQ query+0(FP), AX
	MOVQ ref+8(FP), BX

	// 16-byte unaligned loads
	VMOVDQU (AX), X0
	VMOVDQU (BX), X1

	// abs diff = max - min (unsigned)
	VPMINUB X1, X0, X2  // X2 = min(X0, X1)
	VPMAXUB X1, X0, X3  // X3 = max(X0, X1)
	VPSUBB  X2, X3, X4  // X4 = |q - r|, 16 bytes

	// zero-extend bytes to 16-bit words: 16 bytes (xmm) -> 16 words (ymm)
	VPMOVZXBW X4, Y5

	// pmaddwd: result[i] = (Y5[2i] * Y5[2i]) + (Y5[2i+1] * Y5[2i+1])
	// 8 dwords, sum = total squared distance.
	VPMADDWD Y5, Y5, Y6

	// horizontal sum: 8 dwords -> 1 uint32
	VEXTRACTI128 $1, Y6, X7   // X7 = high 4 dwords of Y6
	VPADDD X6, X7, X8         // X8 = [a+e, b+f, c+g, d+h]
	VPSHUFD $0x4E, X8, X9     // X9 = [c+g, d+h, a+e, b+f] (swap halves)
	VPADDD X8, X9, X8         // X8 lower 2 dwords: [a+e+c+g, b+f+d+h, ...]
	VPSHUFD $0xB1, X8, X9     // X9 = [b+f+d+h, a+e+c+g, ...] (swap adjacent)
	VPADDD X8, X9, X8         // X8 low dword: sum
	VMOVD X8, AX              // AX = sum

	VZEROUPPER

	MOVL AX, ret+16(FP)
	RET

/* AVX2 squared Euclidean distance routines.
 *
 * Port of distance_amd64.s (Plan 9 / Go assembler) → GAS for C linkage.
 * Both functions: 16-byte unaligned input vectors, byte-level operations,
 * sum-of-squares via PMADDWD + horizontal sum. Result in EAX.
 *
 * Calling convention: System V AMD64 ABI.
 *   distance_sq(q, ref): %rdi = q, %rsi = ref
 *   bound_dist_sq(q, min, max): %rdi = q, %rsi = min, %rdx = max
 */

.section .text

.globl distance_sq
.type  distance_sq, @function
distance_sq:
	vmovdqu  (%rdi), %xmm0
	vmovdqu  (%rsi), %xmm1

	vpminub  %xmm1, %xmm0, %xmm2
	vpmaxub  %xmm1, %xmm0, %xmm3
	vpsubb   %xmm2, %xmm3, %xmm4

	vpmovzxbw %xmm4, %ymm5
	vpmaddwd  %ymm5, %ymm5, %ymm6

	vextracti128 $1, %ymm6, %xmm7
	vpaddd   %xmm7, %xmm6, %xmm6
	vpshufd  $0x4e, %xmm6, %xmm7
	vpaddd   %xmm7, %xmm6, %xmm6
	vpshufd  $0xb1, %xmm6, %xmm7
	vpaddd   %xmm7, %xmm6, %xmm6
	vmovd    %xmm6, %eax
	vzeroupper
	ret
.size distance_sq, .-distance_sq


.globl bound_dist_sq
.type  bound_dist_sq, @function
bound_dist_sq:
	vmovdqu  (%rdi), %xmm0       /* query */
	vmovdqu  (%rsi), %xmm1       /* min_box */
	vmovdqu  (%rdx), %xmm2       /* max_box */

	/* xmm3 = max(0, min_box - query) per byte */
	vpsubusb %xmm0, %xmm1, %xmm3
	/* xmm4 = max(0, query - max_box) per byte */
	vpsubusb %xmm2, %xmm0, %xmm4
	/* xmm5 = per-byte edge distance (only one of xmm3[d], xmm4[d] is non-zero) */
	vpmaxub  %xmm3, %xmm4, %xmm5

	vpmovzxbw %xmm5, %ymm6
	vpmaddwd  %ymm6, %ymm6, %ymm7

	vextracti128 $1, %ymm7, %xmm8
	vpaddd   %xmm8, %xmm7, %xmm7
	vpshufd  $0x4e, %xmm7, %xmm8
	vpaddd   %xmm8, %xmm7, %xmm7
	vpshufd  $0xb1, %xmm7, %xmm8
	vpaddd   %xmm8, %xmm7, %xmm7
	vmovd    %xmm7, %eax
	vzeroupper
	ret
.size bound_dist_sq, .-bound_dist_sq

/* Non-executable stack hint for security tools. */
.section .note.GNU-stack,"",%progbits

/* AVX2 batched squared Euclidean distance.
 *
 * distance_sq_batch4(query, base, out):
 *   %rdi = query (16 bytes)
 *   %rsi = base  (64 bytes — four contiguous records)
 *   %rdx = out   (uint32_t[4])
 *
 * Processes four 16-byte records in parallel using independent ymm
 * registers so the out-of-order engine can pipeline. Hot path of
 * the k-NN scan over a partition.
 *
 * System V AMD64 ABI; clobbers xmm0..xmm15. Uses vzeroupper on exit.
 */

.section .text
.globl distance_sq_batch4
.type  distance_sq_batch4, @function
distance_sq_batch4:
	vmovdqu  (%rdi), %xmm0           /* q once; reused 4 times */

	/* Record 0 → partial sums in ymm6 */
	vmovdqu  0(%rsi),  %xmm1
	vpminub  %xmm1,    %xmm0, %xmm2
	vpmaxub  %xmm1,    %xmm0, %xmm3
	vpsubb   %xmm2,    %xmm3, %xmm4
	vpmovzxbw %xmm4,   %ymm5
	vpmaddwd %ymm5,    %ymm5, %ymm6

	/* Record 1 → ymm10 */
	vmovdqu  16(%rsi), %xmm7
	vpminub  %xmm7,    %xmm0, %xmm8
	vpmaxub  %xmm7,    %xmm0, %xmm9
	vpsubb   %xmm8,    %xmm9, %xmm1
	vpmovzxbw %xmm1,   %ymm11
	vpmaddwd %ymm11,   %ymm11, %ymm10

	/* Record 2 → ymm13 */
	vmovdqu  32(%rsi), %xmm2
	vpminub  %xmm2,    %xmm0, %xmm3
	vpmaxub  %xmm2,    %xmm0, %xmm4
	vpsubb   %xmm3,    %xmm4, %xmm5
	vpmovzxbw %xmm5,   %ymm12
	vpmaddwd %ymm12,   %ymm12, %ymm13

	/* Record 3 → ymm15 */
	vmovdqu  48(%rsi), %xmm7
	vpminub  %xmm7,    %xmm0, %xmm8
	vpmaxub  %xmm7,    %xmm0, %xmm9
	vpsubb   %xmm8,    %xmm9, %xmm1
	vpmovzxbw %xmm1,   %ymm14
	vpmaddwd %ymm14,   %ymm14, %ymm15

	/* hsum ymm6 → out[0] */
	vextracti128 $1, %ymm6, %xmm2
	vpaddd   %xmm2,    %xmm6, %xmm6
	vpshufd  $0x4e,    %xmm6, %xmm2
	vpaddd   %xmm2,    %xmm6, %xmm6
	vpshufd  $0xb1,    %xmm6, %xmm2
	vpaddd   %xmm2,    %xmm6, %xmm6
	vmovd    %xmm6,    %eax
	movl     %eax,     0(%rdx)

	/* hsum ymm10 → out[1] */
	vextracti128 $1, %ymm10, %xmm2
	vpaddd   %xmm2,    %xmm10, %xmm10
	vpshufd  $0x4e,    %xmm10, %xmm2
	vpaddd   %xmm2,    %xmm10, %xmm10
	vpshufd  $0xb1,    %xmm10, %xmm2
	vpaddd   %xmm2,    %xmm10, %xmm10
	vmovd    %xmm10,   %eax
	movl     %eax,     4(%rdx)

	/* hsum ymm13 → out[2] */
	vextracti128 $1, %ymm13, %xmm2
	vpaddd   %xmm2,    %xmm13, %xmm13
	vpshufd  $0x4e,    %xmm13, %xmm2
	vpaddd   %xmm2,    %xmm13, %xmm13
	vpshufd  $0xb1,    %xmm13, %xmm2
	vpaddd   %xmm2,    %xmm13, %xmm13
	vmovd    %xmm13,   %eax
	movl     %eax,     8(%rdx)

	/* hsum ymm15 → out[3] */
	vextracti128 $1, %ymm15, %xmm2
	vpaddd   %xmm2,    %xmm15, %xmm15
	vpshufd  $0x4e,    %xmm15, %xmm2
	vpaddd   %xmm2,    %xmm15, %xmm15
	vpshufd  $0xb1,    %xmm15, %xmm2
	vpaddd   %xmm2,    %xmm15, %xmm15
	vmovd    %xmm15,   %eax
	movl     %eax,     12(%rdx)

	vzeroupper
	ret
.size distance_sq_batch4, .-distance_sq_batch4

.section .note.GNU-stack,"",%progbits

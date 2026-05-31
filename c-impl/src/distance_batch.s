/* AVX2 batched squared Euclidean distance — i16 (Phase 2, format v13).
 *
 * distance_sq_batch4(query, base, out):
 *   %rdi = query (32 bytes, 16 i16)
 *   %rsi = base  (128 bytes — four contiguous 32-byte i16 records)
 *   %rdx = out   (uint32_t[4])
 *
 * Four records in parallel via independent ymm accumulators so the OoO engine
 * pipelines. Each lane diff in [-10000,10000]; vpmaddwd pair-sum <= 2e8;
 * 8-lane total <= 5e8 < INT32_MAX. System V AMD64 ABI; vzeroupper on exit.
 */

.section .text
.globl distance_sq_batch4
.type  distance_sq_batch4, @function
distance_sq_batch4:
	vmovdqu  (%rdi), %ymm0           /* q once; reused 4 times */

	/* Record 0 -> ymm6 */
	vmovdqu  0(%rsi),   %ymm1
	vpsubw   %ymm1,     %ymm0, %ymm2
	vpmaddwd %ymm2,     %ymm2, %ymm6

	/* Record 1 -> ymm10 */
	vmovdqu  32(%rsi),  %ymm3
	vpsubw   %ymm3,     %ymm0, %ymm4
	vpmaddwd %ymm4,     %ymm4, %ymm10

	/* Record 2 -> ymm13 */
	vmovdqu  64(%rsi),  %ymm5
	vpsubw   %ymm5,     %ymm0, %ymm7
	vpmaddwd %ymm7,     %ymm7, %ymm13

	/* Record 3 -> ymm15 */
	vmovdqu  96(%rsi),  %ymm8
	vpsubw   %ymm8,     %ymm0, %ymm9
	vpmaddwd %ymm9,     %ymm9, %ymm15

	/* hsum ymm6 -> out[0] */
	vextracti128 $1, %ymm6, %xmm2
	vpaddd   %xmm2,    %xmm6, %xmm6
	vpshufd  $0x4e,    %xmm6, %xmm2
	vpaddd   %xmm2,    %xmm6, %xmm6
	vpshufd  $0xb1,    %xmm6, %xmm2
	vpaddd   %xmm2,    %xmm6, %xmm6
	vmovd    %xmm6,    %eax
	movl     %eax,     0(%rdx)

	/* hsum ymm10 -> out[1] */
	vextracti128 $1, %ymm10, %xmm2
	vpaddd   %xmm2,    %xmm10, %xmm10
	vpshufd  $0x4e,    %xmm10, %xmm2
	vpaddd   %xmm2,    %xmm10, %xmm10
	vpshufd  $0xb1,    %xmm10, %xmm2
	vpaddd   %xmm2,    %xmm10, %xmm10
	vmovd    %xmm10,   %eax
	movl     %eax,     4(%rdx)

	/* hsum ymm13 -> out[2] */
	vextracti128 $1, %ymm13, %xmm2
	vpaddd   %xmm2,    %xmm13, %xmm13
	vpshufd  $0x4e,    %xmm13, %xmm2
	vpaddd   %xmm2,    %xmm13, %xmm13
	vpshufd  $0xb1,    %xmm13, %xmm2
	vpaddd   %xmm2,    %xmm13, %xmm13
	vmovd    %xmm13,   %eax
	movl     %eax,     8(%rdx)

	/* hsum ymm15 -> out[3] */
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

/* AVX2 squared Euclidean distance routines — i16 (Phase 2, format v13).
 *
 * Vectors are 16 int16 lanes (14 real dims + 2 zero pad) = 32 bytes = one ymm.
 * Values are in [-5000, 5000] (I16_SCALE=5000; sentinel -5000), so per-lane
 * diff is in [-10000, 10000] (fits i16), diff^2 <= 1e8, vpmaddwd pair-sum
 * <= 2e8 per i32 lane, full 8-lane sum <= 5e8 < INT32_MAX. Result in EAX.
 *
 * System V AMD64 ABI.
 *   distance_sq(q, ref):            %rdi=q, %rsi=ref
 *   bound_dist_sq(q, min, max):     %rdi=q, %rsi=min, %rdx=max
 */

.section .text

.globl distance_sq
.type  distance_sq, @function
distance_sq:
	vmovdqu  (%rdi), %ymm0          /* q   : 16 i16 */
	vmovdqu  (%rsi), %ymm1          /* ref : 16 i16 */
	vpsubw   %ymm1, %ymm0, %ymm2    /* diff = q - ref (signed) */
	vpmaddwd %ymm2, %ymm2, %ymm3    /* 8 i32: diff[2i]^2 + diff[2i+1]^2 */

	vextracti128 $1, %ymm3, %xmm4
	vpaddd   %xmm4, %xmm3, %xmm3
	vpshufd  $0x4e, %xmm3, %xmm4
	vpaddd   %xmm4, %xmm3, %xmm3
	vpshufd  $0xb1, %xmm3, %xmm4
	vpaddd   %xmm4, %xmm3, %xmm3
	vmovd    %xmm3, %eax
	vzeroupper
	ret
.size distance_sq, .-distance_sq


.globl bound_dist_sq
.type  bound_dist_sq, @function
bound_dist_sq:
	vmovdqu  (%rdi), %ymm0          /* query   */
	vmovdqu  (%rsi), %ymm1          /* min_box */
	vmovdqu  (%rdx), %ymm2          /* max_box */

	vpsubw   %ymm0, %ymm1, %ymm3    /* min - q  (>0 when q < min)        */
	vpsubw   %ymm2, %ymm0, %ymm4    /* q - max  (>0 when q > max)        */
	vpmaxsw  %ymm4, %ymm3, %ymm5    /* per-lane max(min-q, q-max)        */
	vpxor    %ymm6, %ymm6, %ymm6
	vpmaxsw  %ymm6, %ymm5, %ymm5    /* clamp negatives (inside box) to 0 */
	vpmaddwd %ymm5, %ymm5, %ymm7    /* edge^2, paired into 8 i32         */

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

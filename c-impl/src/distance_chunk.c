/* AVX2 leaf-chunk distance kernel (SEARCH_V2).
 *
 * One LeafChunk holds 8 records in dimension-pair SoA: pairs[p] is a 32-byte
 * row of [r0.d2p, r0.d2p+1, r1.d2p, r1.d2p+1, ...]. Per pair row: one 32B
 * load + vpsubw + vpmaddwd + vpaddd accumulates (d2p^2 + d2p+1^2) for all 8
 * records VERTICALLY — no per-record horizontal reduction (the cost that
 * dominates the v1 AoS batch-4 kernel). 7 rows -> 8 final distances stored
 * with a single vmovdqu. ~4 instructions per record vs ~10-12 in v1. */

#include <immintrin.h>

#include "distance_chunk.h"

void chunk_prepack_query(const int16_t q[RECORD_STRIDE], int32_t qp[7])
{
	for (int p = 0; p < 7; p++)
		qp[p] = (int32_t)((uint32_t)(uint16_t)q[2*p] |
		                  ((uint32_t)(uint16_t)q[2*p + 1] << 16));
}

void chunk_distances(const int32_t qp[7], const LeafChunk *c, uint32_t out[8])
{
	__m256i acc = _mm256_setzero_si256();
	for (int p = 0; p < 7; p++) {
		__m256i recs = _mm256_loadu_si256((const __m256i *)c->pairs[p]);
		__m256i qbar = _mm256_set1_epi32(qp[p]);
		__m256i diff = _mm256_sub_epi16(qbar, recs);
		__m256i sq   = _mm256_madd_epi16(diff, diff);
		acc = _mm256_add_epi32(acc, sq);
	}
	_mm256_storeu_si256((__m256i *)out, acc);
}

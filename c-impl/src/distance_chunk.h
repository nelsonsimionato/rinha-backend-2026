#ifndef DISTANCE_CHUNK_H
#define DISTANCE_CHUNK_H

#include "compat.h"
#include "index.h"

/* Pre-pack the query for the chunk kernel: qp[p] = lanes (2p, 2p+1) packed
 * into one i32 (low half = lane 2p), broadcast-ready for vpmaddwd. Lanes
 * 14,15 are always 0 (query memset + builder pad) so 7 pairs suffice. */
void chunk_prepack_query(const int16_t q[RECORD_STRIDE], int32_t qp[7]);

/* Distances from the pre-packed query to all 8 records of one chunk.
 * out[j] = sum over 14 dims of (q[d] - rec_j[d])^2, exact in u32:
 * vpmaddwd i32 lanes <= 5e8 (sentinel dims 5,6 sit in different pairs),
 * 7-pair vertical sum <= 2.0e9 < 2^31 (same contract as distance.s). */
void chunk_distances(const int32_t qp[7], const LeafChunk *c, uint32_t out[8]);

#endif /* DISTANCE_CHUNK_H */

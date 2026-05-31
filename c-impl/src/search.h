#ifndef SEARCH_H
#define SEARCH_H

#include "compat.h"

typedef struct {
	uint32_t dist_sq;
	int32_t  node_idx;   /* record index into data[]/is_fraud[] */
} Neighbor;

typedef struct {
	Neighbor q[K];
	int      q_count;
	uint32_t stack[KD_STACK_SIZE];  /* KD traversal stack (node indices) */
} SearchState;

/* Exact k-NN over the balanced KD-tree (format v12). Best-first descent with
 * axis-aligned bounding-box pruning (AVX2 bound_dist_sq), AVX2 batch-4 leaf
 * scan. Writes the top-K (ascending dist_sq) into `out`. Recall is exact w.r.t.
 * the uint8-quantized vectors. */
void search_knn(const uint8_t query[RECORD_STRIDE], SearchState *st,
                Neighbor out[K]);

#endif /* SEARCH_H */

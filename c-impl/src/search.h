#ifndef SEARCH_H
#define SEARCH_H

#include "compat.h"

typedef struct {
	uint32_t dist_sq;
	int32_t  node_idx;
} Neighbor;

typedef struct {
	uint32_t bound;
	uint16_t idx;
} Candidate;

typedef struct {
	Neighbor  q[K];
	int       q_count;
	Candidate cands[MAX_CANDIDATES];
	int       n_cands;
} SearchState;

/* Adaptive bound-pruned k-NN. Writes top-K into `out`.
 * Hot path is the matching partition (scanned via AVX2 distance_sq);
 * fallback into bound-pruned exhaustive when matching < threshold. */
void search_knn(const uint8_t query[RECORD_STRIDE], SearchState *st,
                Neighbor out[K]);

#endif /* SEARCH_H */

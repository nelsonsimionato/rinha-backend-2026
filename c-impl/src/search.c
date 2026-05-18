#include "search.h"
#include "distance.h"
#include "index.h"
#include "partition.h"

/* Insertion sort: maintain ascending dist_sq among first K items. */
ALWAYS_INLINE void knn_push(SearchState *st, uint32_t d, int32_t idx)
{
	if (st->q_count < K) {
		st->q[st->q_count].dist_sq = d;
		st->q[st->q_count].node_idx = idx;
		st->q_count++;
		for (int i = st->q_count - 1; i > 0 && st->q[i].dist_sq < st->q[i-1].dist_sq; i--) {
			Neighbor tmp = st->q[i]; st->q[i] = st->q[i-1]; st->q[i-1] = tmp;
		}
	} else if (d < st->q[K-1].dist_sq) {
		st->q[K-1].dist_sq = d;
		st->q[K-1].node_idx = idx;
		for (int i = K - 1; i > 0 && st->q[i].dist_sq < st->q[i-1].dist_sq; i--) {
			Neighbor tmp = st->q[i]; st->q[i] = st->q[i-1]; st->q[i-1] = tmp;
		}
	}
}

/* Bounded insertion into candidates[] sorted ascending by bound; discards
 * candidates with bound >= worst-of-current. Cap at MAX_CANDIDATES. */
ALWAYS_INLINE void cand_push(SearchState *st, uint32_t b, uint16_t idx)
{
	if (st->n_cands < MAX_CANDIDATES) {
		st->cands[st->n_cands].bound = b;
		st->cands[st->n_cands].idx   = idx;
		st->n_cands++;
		for (int i = st->n_cands - 1; i > 0 && st->cands[i].bound < st->cands[i-1].bound; i--) {
			Candidate tmp = st->cands[i]; st->cands[i] = st->cands[i-1]; st->cands[i-1] = tmp;
		}
		return;
	}
	if (b >= st->cands[MAX_CANDIDATES - 1].bound) return;
	st->cands[MAX_CANDIDATES - 1].bound = b;
	st->cands[MAX_CANDIDATES - 1].idx   = idx;
	for (int i = MAX_CANDIDATES - 1; i > 0 && st->cands[i].bound < st->cands[i-1].bound; i--) {
		Candidate tmp = st->cands[i]; st->cands[i] = st->cands[i-1]; st->cands[i-1] = tmp;
	}
}

/* Records ahead to prefetch. 16 * 16B = 256 B = 4 cache lines. Matches the
 * latency-to-DRAM (~200 cycles) vs ~5 records/cycle throughput. */
#define PREFETCH_AHEAD 16

ALWAYS_INLINE void scan_partition(const uint8_t *query, uint16_t pidx, SearchState *st)
{
	uint32_t start = partition_offsets[pidx];
	uint32_t size  = partition_sizes[pidx];
	if (size == 0) return;
	uint32_t end = start + size;

	uint32_t r = start;
	for (; r + 4 <= end; r += 4) {
		__builtin_prefetch(&data[(size_t)(r + PREFETCH_AHEAD) * RECORD_STRIDE], 0, 1);
		uint32_t d[4];
		distance_sq_batch4(query, &data[(size_t)r * RECORD_STRIDE], d);
		knn_push(st, d[0], (int32_t)(r));
		knn_push(st, d[1], (int32_t)(r + 1));
		knn_push(st, d[2], (int32_t)(r + 2));
		knn_push(st, d[3], (int32_t)(r + 3));
	}
	for (; r < end; r++) {
		uint32_t d = distance_sq(query, &data[(size_t)r * RECORD_STRIDE]);
		knn_push(st, d, (int32_t)r);
	}
}

void search_knn(const uint8_t query[RECORD_STRIDE], SearchState *st,
                Neighbor out[K])
{
	st->q_count  = 0;
	st->n_cands  = 0;

	uint16_t match_key = partition_key(query);
	uint32_t match_size = partition_sizes[match_key];
	scan_partition(query, match_key, st);

	if (match_size < SKIP_BOUND_PRUNE_IF_MATCHING_AT_LEAST) {
		/* Sparse matching: fall through to exhaustive bound-pruned search. */
		uint32_t worst = ~(uint32_t)0;
		if (st->q_count >= K) worst = st->q[K-1].dist_sq;

		for (uint32_t i = 0; i < non_empty_count; i++) {
			uint16_t pidx = non_empty_partitions[i];
			if (pidx == match_key) continue;
			uint32_t b = bound_dist_sq(query,
			                            &partition_min[(size_t)pidx * RECORD_STRIDE],
			                            &partition_max[(size_t)pidx * RECORD_STRIDE]);
			if (b < worst) cand_push(st, b, pidx);
		}

		for (int i = 0; i < st->n_cands; i++) {
			if (st->q_count >= K && st->cands[i].bound >= st->q[K-1].dist_sq) break;
			scan_partition(query, st->cands[i].idx, st);
		}
	}

	for (int i = 0; i < K; i++) out[i] = st->q[i];
}

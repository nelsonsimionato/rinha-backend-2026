#include "search.h"
#include "distance.h"
#include "index.h"

STATIC_ASSERT(sizeof(KDNode) == KD_NODE_BYTES, "KDNode must be 40 bytes (on-disk layout)");

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

/* Records ahead to prefetch. 8 * 16B = 128 B = 2 cache lines. */
#define PREFETCH_AHEAD 8

/* Scan a leaf's [start, start+count) records into the top-K queue. */
ALWAYS_INLINE void scan_leaf(const uint8_t *query, uint32_t start, uint32_t count,
                             SearchState *st)
{
	uint32_t end = start + count;
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

/* Exact KD k-NN. Iterative best-effort DFS: at each internal node, descend the
 * child whose bounding box is nearer first (tightens the K-th distance early),
 * and prune a node when its box lower-bound is already >= the current K-th.
 * The stack holds at most the tree height (<< KD_STACK_SIZE). */
void search_knn(const uint8_t query[RECORD_STRIDE], SearchState *st,
                Neighbor out[K])
{
	st->q_count = 0;
	if (UNLIKELY(kd_node_count == 0)) {
		for (int i = 0; i < K; i++) { out[i].dist_sq = ~0u; out[i].node_idx = 0; }
		return;
	}

	int sp = 0;
	st->stack[sp++] = 0; /* root */

	while (sp > 0) {
		uint32_t ni = st->stack[--sp];
		const KDNode *nd = &kd_nodes[ni];

		/* Prune: if the box can't beat the current K-th neighbor, skip. */
		if (st->q_count >= K) {
			uint32_t b = bound_dist_sq(query, nd->bmin, nd->bmax);
			if (b >= st->q[K-1].dist_sq) continue;
		}

		if (nd->b & KD_LEAF_FLAG) {
			scan_leaf(query, nd->a, nd->b & KD_COUNT_MASK, st);
			continue;
		}

		/* Internal: order children by box lower-bound, push far then near so
		 * the near child is popped (and scanned) first. */
		uint32_t L = nd->a, R = nd->b;
		uint32_t bl = bound_dist_sq(query, kd_nodes[L].bmin, kd_nodes[L].bmax);
		uint32_t br = bound_dist_sq(query, kd_nodes[R].bmin, kd_nodes[R].bmax);
		if (bl <= br) {
			st->stack[sp++] = R;
			st->stack[sp++] = L;
		} else {
			st->stack[sp++] = L;
			st->stack[sp++] = R;
		}
	}

	for (int i = 0; i < K; i++) out[i] = st->q[i];
}

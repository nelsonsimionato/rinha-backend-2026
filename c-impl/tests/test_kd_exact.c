/* Exactness validation: KD-tree search_knn() must return the SAME 5 nearest
 * distances as exhaustive brute force over the uint8-quantized 3M dataset.
 * (Which specific record wins a distance tie may differ by visit order, but the
 *  multiset of the 5 smallest distances is invariant — that is what we check,
 *  and we additionally report any fraud-count divergence.)
 *
 * Build (from c-impl/):
 *   gcc -O3 -mavx2 -march=haswell -Isrc -o tests/test_kd_exact tests/test_kd_exact.c \
 *       src/index.o src/search.o src/distance.o src/distance_batch.o -static -lm
 * Run from c-impl/:  ./tests/test_kd_exact [Nqueries]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/index.h"
#include "../src/search.h"
#include "../src/distance.h"

static void brute_top5(const int16_t *q, uint32_t dists[K], int fraud_out[1])
{
	uint32_t best[K]; int32_t bidx[K]; int cnt = 0;
	uint32_t n = total_records;
	for (uint32_t r = 0; r < n; r++) {
		uint32_t d = distance_sq(q, &data[(size_t)r * RECORD_STRIDE]);
		if (cnt < K) {
			best[cnt] = d; bidx[cnt] = (int32_t)r; cnt++;
			for (int i = cnt-1; i > 0 && best[i] < best[i-1]; i--) {
				uint32_t td=best[i]; best[i]=best[i-1]; best[i-1]=td;
				int32_t ti=bidx[i]; bidx[i]=bidx[i-1]; bidx[i-1]=ti;
			}
		} else if (d < best[K-1]) {
			best[K-1] = d; bidx[K-1] = (int32_t)r;
			for (int i = K-1; i > 0 && best[i] < best[i-1]; i--) {
				uint32_t td=best[i]; best[i]=best[i-1]; best[i-1]=td;
				int32_t ti=bidx[i]; bidx[i]=bidx[i-1]; bidx[i-1]=ti;
			}
		}
	}
	int f = 0;
	for (int i = 0; i < K; i++) { dists[i] = best[i]; f += is_fraud[bidx[i]]; }
	fraud_out[0] = f;
}

int main(int argc, char **argv)
{
	int Q = (argc > 1) ? atoi(argv[1]) : 500;
	/* Build the SEARCH_V2 arrays too: every query is checked 3-way
	 * (brute force / v1 / v2). The flag is toggled per call below. */
	setenv("SEARCH_V2", "1", 1);
	if (index_load("../resources/index.bin") != 0) { fprintf(stderr,"load failed\n"); return 1; }
	int have_v2 = g_search_v2;
	if (!have_v2) fprintf(stderr, "WARN: SEARCH_V2 arrays unavailable, v2 checks skipped\n");

	srand(20260531);
	uint32_t n = total_records;
	int dist_mismatch = 0, fraud_mismatch = 0;
	SearchState st;
	Neighbor out[K];

	for (int t = 0; t < Q; t++) {
		uint32_t row = (uint32_t)(((unsigned long)rand()*1103515245UL + rand()) % n);
		int16_t q[RECORD_STRIDE];
		memcpy(q, &data[(size_t)row * RECORD_STRIDE], RECORD_STRIDE * sizeof(int16_t));
		/* Perturb half the queries by ±a few hundred i16 levels on a couple of dims
		 * so they are not exact record matches (exercises real pruning paths). */
		if (t & 1) {
			for (int d = 0; d < 6; d++) {
				int dim = (row + d*7) % DIMENSIONS;
				int delta = (((int)((row >> d) & 7)) - 3) * 100;   /* -300..+400 */
				int v = (int)q[dim] + delta;
				if (v < -5000) v = -5000; if (v > 5000) v = 5000;
				q[dim] = (int16_t)v;
			}
		}

		g_search_v2 = 0;                      /* v1 path */
		search_knn(q, &st, out);
		uint32_t kd_d[K]; int kd_f = 0;
		for (int i = 0; i < K; i++) { kd_d[i] = out[i].dist_sq; kd_f += is_fraud[out[i].node_idx]; }

		uint32_t v2_d[K]; int v2_f = 0;
		if (have_v2) {
			g_search_v2 = 1;                  /* v2 path */
			Neighbor out2[K];
			search_knn(q, &st, out2);
			for (int i = 0; i < K; i++) { v2_d[i] = out2[i].dist_sq; v2_f += is_fraud[out2[i].node_idx]; }
		}

		uint32_t bf_d[K]; int bf_f;
		brute_top5(q, bf_d, &bf_f);

		int dm = 0;
		for (int i = 0; i < K; i++) if (kd_d[i] != bf_d[i]) dm = 1;
		if (have_v2)
			for (int i = 0; i < K; i++) if (v2_d[i] != bf_d[i]) dm = 1;
		if (dm) {
			dist_mismatch++;
			if (dist_mismatch <= 5) {
				fprintf(stderr, "DIST MISMATCH q#%d row=%u\n  kd =", t, row);
				for (int i=0;i<K;i++) fprintf(stderr," %u",kd_d[i]);
				if (have_v2) {
					fprintf(stderr, "\n  v2 =");
					for (int i=0;i<K;i++) fprintf(stderr," %u",v2_d[i]);
				}
				fprintf(stderr, "\n  bf =");
				for (int i=0;i<K;i++) fprintf(stderr," %u",bf_d[i]);
				fprintf(stderr, "\n");
			}
		}
		if (kd_f != bf_f) fraud_mismatch++;
		if (have_v2 && v2_f != bf_f) fraud_mismatch++;
	}

	printf("KD-vs-bruteforce over %d queries (%s): dist-set mismatches=%d, fraud-count mismatches=%d\n",
	       Q, have_v2 ? "v1+v2 both checked" : "v1 only", dist_mismatch, fraud_mismatch);
	if (dist_mismatch == 0 && fraud_mismatch == 0) {
		printf("EXACT RECALL CONFIRMED — %s return identical 5-NN distances to brute force.\n",
		       have_v2 ? "v1 AND v2 paths" : "KD-tree");
		return 0;
	}
	printf("FAIL: KD search is NOT exact.\n");
	return 1;
}

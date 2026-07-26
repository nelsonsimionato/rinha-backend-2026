/* search_knn latency microbench: v1 (AoS batch-4) vs v2 (SoA chunks).
 * Prints p50/p99/max per path; always exits 0 (perf harness, not a gate).
 * Run from c-impl/:  ./tests/bench_search [Nqueries]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../src/index.h"
#include "../src/search.h"

static inline uint64_t mono_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int cmp_u64(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
	return x < y ? -1 : x > y ? 1 : 0;
}

static void run_path(const char *name, int v2, int Q, int16_t (*queries)[RECORD_STRIDE],
                     uint64_t *lat, volatile uint32_t *sink)
{
	SearchState st;
	Neighbor out[K];
	g_search_v2 = v2;
	/* warm the path */
	for (int t = 0; t < 2000 && t < Q; t++) search_knn(queries[t], &st, out);
	for (int t = 0; t < Q; t++) {
		uint64_t t0 = mono_ns();
		search_knn(queries[t], &st, out);
		lat[t] = mono_ns() - t0;
		*sink += (uint32_t)out[0].node_idx;
	}
	qsort(lat, (size_t)Q, sizeof(uint64_t), cmp_u64);
	printf("%s: p50=%.2fus p90=%.2fus p99=%.2fus p999=%.2fus max=%.2fus\n",
	       name, lat[Q/2]/1e3, lat[(int)(Q*0.90)]/1e3, lat[(int)(Q*0.99)]/1e3,
	       lat[(int)(Q*0.999)]/1e3, lat[Q-1]/1e3);
}

int main(int argc, char **argv)
{
	int Q = (argc > 1) ? atoi(argv[1]) : 50000;
	setenv("SEARCH_V2", "1", 1);
	if (index_load("../resources/index.bin") != 0) { fprintf(stderr,"load failed\n"); return 0; }
	if (!g_search_v2) { fprintf(stderr, "v2 arrays unavailable; bench aborted\n"); return 0; }

	/* Perturbed-record queries (same recipe as test_kd_exact). */
	int16_t (*queries)[RECORD_STRIDE] = malloc((size_t)Q * sizeof(*queries));
	uint64_t *lat = malloc((size_t)Q * sizeof(uint64_t));
	if (!queries || !lat) return 0;
	srand(20260605);
	for (int t = 0; t < Q; t++) {
		uint32_t row = (uint32_t)(((unsigned long)rand()*1103515245UL + rand()) % total_records);
		memcpy(queries[t], &data[(size_t)row * RECORD_STRIDE], sizeof(*queries));
		for (int d = 0; d < 6; d++) {
			int dim = (row + d*7) % DIMENSIONS;
			int delta = (((int)((row >> d) & 7)) - 3) * 100;
			int v = (int)queries[t][dim] + delta;
			if (v < -5000) v = -5000;
			if (v > 5000) v = 5000;
			queries[t][dim] = (int16_t)v;
		}
	}

	volatile uint32_t sink = 0;
	run_path("v1(AoS batch4) ", 0, Q, queries, lat, &sink);
	run_path("v2(SoA chunks) ", 1, Q, queries, lat, &sink);
	run_path("v1(again)      ", 0, Q, queries, lat, &sink);
	run_path("v2(again)      ", 1, Q, queries, lat, &sink);
	(void)sink;
	return 0;
}

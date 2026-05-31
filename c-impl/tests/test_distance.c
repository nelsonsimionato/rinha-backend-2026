/* Tests the AVX2 i16 distance_sq / bound_dist_sq / distance_sq_batch4 kernels
 * (format v13) against slow scalar references. Values mirror the real index:
 * 16 i16 lanes in [-5000, 5000] (sentinel -5000), lanes 14,15 = 0. */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "../src/distance.h"

static uint32_t distance_sq_ref(const int16_t *q, const int16_t *r)
{
	uint32_t sum = 0;
	for (int i = 0; i < 16; i++) {
		int32_t d = (int32_t)q[i] - (int32_t)r[i];
		sum += (uint32_t)(d * d);
	}
	return sum;
}

static uint32_t bound_dist_sq_ref(const int16_t *q, const int16_t *mn, const int16_t *mx)
{
	uint32_t sum = 0;
	for (int i = 0; i < 16; i++) {
		int32_t qi = q[i], mni = mn[i], mxi = mx[i], d;
		if (qi < mni)      d = mni - qi;
		else if (qi > mxi) d = qi - mxi;
		else               d = 0;
		sum += (uint32_t)(d * d);
	}
	return sum;
}

/* random i16 lane in [-5000, 5000] */
static int16_t rnd(uint32_t *seed)
{
	*seed = *seed * 1103515245u + 12345u;
	return (int16_t)((int)((*seed >> 12) % 10001) - 5000);
}

static int test_edge_cases(void)
{
	struct { int16_t q[16], r[16]; uint32_t want; const char *name; } cases[] = {
		{ {0}, {0}, 0, "all zeros" },
		{ {5000,5000,5000,5000,5000,5000,5000,5000,5000,5000,5000,5000,5000,5000,0,0},
		  {5000,5000,5000,5000,5000,5000,5000,5000,5000,5000,5000,5000,5000,5000,0,0},
		  0, "identical max" },
		{ {5000}, {-5000}, 100000000u, "sentinel-vs-max diff at lane 0 (10000^2)" },
		{ {0,0,0,0,0,-5000,5000,0,0,0,0,0,0,0,0,0},
		  {0,0,0,0,0, 5000,5000,0,0,0,0,0,0,0,0,0},
		  100000000u, "sentinel pair at lanes 5,6 (adjacent vpmaddwd pair)" },
		{ {100,100,100,100,100,100,100,100,100,100,100,100,100,100,0,0},
		  {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
		  140000u, "diff 100 across all 14 dims" },
	};
	int n = sizeof(cases)/sizeof(cases[0]);
	for (int i = 0; i < n; i++) {
		uint32_t got = distance_sq(cases[i].q, cases[i].r);
		if (got != cases[i].want) {
			fprintf(stderr, "edge[%d %s] distance_sq got=%u want=%u\n",
				i, cases[i].name, got, cases[i].want);
			return 1;
		}
	}
	printf("edge cases (%d): pass\n", n);
	return 0;
}

static int test_random_distance(int n_cases)
{
	uint32_t seed = 42;
	for (int c = 0; c < n_cases; c++) {
		int16_t q[16] = {0}, r[16] = {0};
		for (int i = 0; i < 14; i++) { q[i] = rnd(&seed); r[i] = rnd(&seed); }
		uint32_t got = distance_sq(q, r), want = distance_sq_ref(q, r);
		if (got != want) { fprintf(stderr, "random distance[%d] got=%u want=%u\n", c, got, want); return 1; }
	}
	printf("random distance_sq (%d): pass\n", n_cases);
	return 0;
}

static int test_random_bound(int n_cases)
{
	uint32_t seed = 1337;
	for (int c = 0; c < n_cases; c++) {
		int16_t q[16] = {0}, mn[16] = {0}, mx[16] = {0};
		for (int i = 0; i < 14; i++) {
			q[i] = rnd(&seed);
			int16_t a = rnd(&seed), b = rnd(&seed);
			if (a < b) { mn[i] = a; mx[i] = b; } else { mn[i] = b; mx[i] = a; }
		}
		uint32_t got = bound_dist_sq(q, mn, mx), want = bound_dist_sq_ref(q, mn, mx);
		if (got != want) { fprintf(stderr, "random bound[%d] got=%u want=%u\n", c, got, want); return 1; }
	}
	printf("random bound_dist_sq (%d): pass\n", n_cases);
	return 0;
}

static int test_random_batch4(int n_batches)
{
	uint32_t seed = 7777;
	for (int c = 0; c < n_batches; c++) {
		int16_t q[16] = {0};
		int16_t base[64] = {0};   /* 4 records × 16 i16 */
		for (int i = 0; i < 14; i++) q[i] = rnd(&seed);
		for (int r = 0; r < 4; r++)
			for (int i = 0; i < 14; i++) base[r*16 + i] = rnd(&seed);
		uint32_t got[4];
		distance_sq_batch4(q, base, got);
		for (int r = 0; r < 4; r++) {
			uint32_t want = distance_sq_ref(q, base + r*16);
			if (got[r] != want) {
				fprintf(stderr, "batch4[%d r=%d] got=%u want=%u\n", c, r, got[r], want);
				return 1;
			}
		}
	}
	printf("random distance_sq_batch4 (%d batches = %d records): pass\n", n_batches, n_batches * 4);
	return 0;
}

int main(void)
{
	if (test_edge_cases())          return 1;
	if (test_random_distance(1000)) return 1;
	if (test_random_bound(1000))    return 1;
	if (test_random_batch4(1000))   return 1;
	printf("ALL DISTANCE TESTS PASS\n");
	return 0;
}

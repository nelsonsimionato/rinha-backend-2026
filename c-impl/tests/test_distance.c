/* Tests AVX2 distance_sq and bound_dist_sq against a slow scalar reference.
 *
 * Run with: ./tests/test_distance
 * Exit 0 if all match; non-zero on first mismatch.
 *
 * The AVX2 routines MUST match the Go runtime byte-for-byte: distance_sq
 * matches Go's distanceSq; bound_dist_sq matches a reference computed the
 * same way (no Go counterpart yet — algorithmically equivalent).
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "../src/distance.h"

/* Reference: scalar implementation, intentionally slow and obvious. */
static uint32_t distance_sq_ref(const uint8_t *q, const uint8_t *r)
{
	uint32_t sum = 0;
	for (int i = 0; i < 16; i++) {
		int32_t d = (int32_t)q[i] - (int32_t)r[i];
		sum += (uint32_t)(d * d);
	}
	return sum;
}

static uint32_t bound_dist_sq_ref(const uint8_t *q,
                                   const uint8_t *mn,
                                   const uint8_t *mx)
{
	uint32_t sum = 0;
	for (int i = 0; i < 16; i++) {
		int32_t qi = (int32_t)q[i];
		int32_t mni = (int32_t)mn[i];
		int32_t mxi = (int32_t)mx[i];
		int32_t d;
		if (qi < mni)        d = mni - qi;
		else if (qi > mxi)   d = qi - mxi;
		else                 d = 0;
		sum += (uint32_t)(d * d);
	}
	return sum;
}

static int test_edge_cases(void)
{
	struct { uint8_t q[16], r[16]; uint32_t want; const char *name; } cases[] = {
		{ {0}, {0}, 0, "all zeros" },
		{ {255,255,255,255,255,255,255,255,255,255,255,255,255,255,0,0},
		  {255,255,255,255,255,255,255,255,255,255,255,255,255,255,0,0},
		  0, "identical max" },
		{ {255}, {0}, 255*255, "max diff at idx 0" },
		{ {0,0,0,0,0,0,0,0,0,0,0,0,0,255,0,0},
		  {0,0,0,0,0,0,0,0,0,0,0,0,0,0,  0,0},
		  255*255, "max diff at idx 13" },
		{ {1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,0},
		  {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
		  14, "diff 1 across all 14 dims" },
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
		uint8_t q[16] = {0}, r[16] = {0};
		/* fill 14 dims; leave 14/15 as zero (padding contract) */
		for (int i = 0; i < 14; i++) {
			seed = seed * 1103515245u + 12345u;
			q[i] = (uint8_t)(seed >> 16);
			seed = seed * 1103515245u + 12345u;
			r[i] = (uint8_t)(seed >> 16);
		}
		uint32_t got  = distance_sq(q, r);
		uint32_t want = distance_sq_ref(q, r);
		if (got != want) {
			fprintf(stderr, "random distance[%d] got=%u want=%u\n", c, got, want);
			return 1;
		}
	}
	printf("random distance_sq (%d): pass\n", n_cases);
	return 0;
}

static int test_random_bound(int n_cases)
{
	uint32_t seed = 1337;
	for (int c = 0; c < n_cases; c++) {
		uint8_t q[16] = {0}, mn[16] = {0}, mx[16] = {0};
		for (int i = 0; i < 14; i++) {
			seed = seed * 1103515245u + 12345u;
			q[i] = (uint8_t)(seed >> 16);
			seed = seed * 1103515245u + 12345u;
			uint8_t a = (uint8_t)(seed >> 16);
			seed = seed * 1103515245u + 12345u;
			uint8_t b = (uint8_t)(seed >> 16);
			if (a < b) { mn[i] = a; mx[i] = b; } else { mn[i] = b; mx[i] = a; }
		}
		uint32_t got  = bound_dist_sq(q, mn, mx);
		uint32_t want = bound_dist_sq_ref(q, mn, mx);
		if (got != want) {
			fprintf(stderr, "random bound[%d] got=%u want=%u\n", c, got, want);
			return 1;
		}
	}
	printf("random bound_dist_sq (%d): pass\n", n_cases);
	return 0;
}

static int test_random_batch4(int n_batches)
{
	uint32_t seed = 7777;
	for (int c = 0; c < n_batches; c++) {
		uint8_t q[16] = {0};
		uint8_t base[64] = {0};
		for (int i = 0; i < 14; i++) {
			seed = seed * 1103515245u + 12345u;
			q[i] = (uint8_t)(seed >> 16);
		}
		for (int r = 0; r < 4; r++) {
			for (int i = 0; i < 14; i++) {
				seed = seed * 1103515245u + 12345u;
				base[r*16 + i] = (uint8_t)(seed >> 16);
			}
		}
		uint32_t got[4];
		distance_sq_batch4(q, base, got);
		for (int r = 0; r < 4; r++) {
			uint32_t want = distance_sq_ref(q, base + r*16);
			if (got[r] != want) {
				fprintf(stderr, "batch4[%d r=%d] got=%u want=%u\n",
					c, r, got[r], want);
				return 1;
			}
		}
	}
	printf("random distance_sq_batch4 (%d batches = %d records): pass\n",
		n_batches, n_batches * 4);
	return 0;
}

int main(void)
{
	if (test_edge_cases())              return 1;
	if (test_random_distance(1000))     return 1;
	if (test_random_bound(1000))        return 1;
	if (test_random_batch4(1000))       return 1;
	printf("ALL DISTANCE TESTS PASS\n");
	return 0;
}

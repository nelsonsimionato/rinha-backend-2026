#ifndef DISTANCE_H
#define DISTANCE_H

#include "compat.h"

/* Squared Euclidean distance between two 16-lane int16 vectors (32 bytes).
 * Lanes 14 and 15 of both `q` and `ref` MUST be 0 (padding contract); they
 * contribute 0 to the sum. Values are i16 in [-5000, 5000]; result fits u32
 * (max 5e8). Implementation: AVX2 in distance.s (vpsubw + vpmaddwd). */
extern uint32_t distance_sq(const int16_t *q, const int16_t *ref);

/* Squared lower-bound Euclidean distance from `q` to an axis-aligned bounding
 * box (min, max, each 16-lane int16). Per lane:
 *   q < min  -> (min - q)^2
 *   q > max  -> (q - max)^2
 *   else     -> 0
 * AVX2: vpsubw + vpmaxsw (edge) + clamp-to-0 + vpmaddwd. */
extern uint32_t bound_dist_sq(const int16_t *q,
                              const int16_t *min_box,
                              const int16_t *max_box);

/* Batched: distance_sq(q, base + 16*i) for i in [0,4) into out[0..3].
 * `base` must point at 4 contiguous 32-byte records (128 bytes valid). */
extern void distance_sq_batch4(const int16_t *q,
                               const int16_t *base,
                               uint32_t      *out);

#endif /* DISTANCE_H */

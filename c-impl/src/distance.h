#ifndef DISTANCE_H
#define DISTANCE_H

#include "compat.h"

/* Squared Euclidean distance between two 16-byte uint8 vectors.
 * Bytes 14 and 15 of both `q` and `ref` MUST be 0 (padding contract);
 * they participate in the sum.
 *
 * Implementation: AVX2 in distance.s (Haswell). Matches the Go runtime's
 * distanceSqAvx in distance_amd64.s byte-for-byte. */
extern uint32_t distance_sq(const uint8_t *q, const uint8_t *ref);

/* Squared lower-bound Euclidean distance from `q` to a partition's
 * axis-aligned bounding box (min, max, each 16-byte uint8).
 * For each dim:
 *   q < min  → (min - q)^2
 *   q > max  → (q - max)^2
 *   else     → 0
 *
 * AVX2: uses VPSUBUSB + VPMAXUB to compute edge distance per byte. */
extern uint32_t bound_dist_sq(const uint8_t *q,
                              const uint8_t *min_box,
                              const uint8_t *max_box);

/* Batched: computes distance_sq(q, base + 16*i) for i in [0,4) into out[0..3].
 * Implementation: distance_batch.s. Caller must ensure base[..63] is valid. */
extern void distance_sq_batch4(const uint8_t *q,
                               const uint8_t *base,
                               uint32_t      *out);

#endif /* DISTANCE_H */

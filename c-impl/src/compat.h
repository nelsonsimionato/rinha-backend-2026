#ifndef COMPAT_H
#define COMPAT_H

#include <stdint.h>
#include <stddef.h>

#define LIKELY(x)   __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)
#define HOT         __attribute__((hot))
#define COLD        __attribute__((cold))
#define ALWAYS_INLINE static inline __attribute__((always_inline))

#define STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)

/* Vector geometry — must match the index builder (tools/build_kdtree.go). */
#define DIMENSIONS        14
#define RECORD_STRIDE     16          /* i16 lanes/record (14 dims + 2 zero pad) = 32 bytes, one ymm */
#define K                 5
#define FORMAT_VERSION    13          /* v13: KD-tree over i16 (Phase 2) */
#define HEADER_SIZE       16

/* i16 quantization (scale 5000). [0,1] -> [0,5000]; null last_transaction
 * (float -1) -> I16_SENTINEL. Lossless for the 5-NN decision (measured 0
 * errors/6000 vs the float oracle) and overflow-safe: worst dist_sq = 5e8 <<
 * INT32_MAX (one ymm holds 16 i16; vpmaddwd pairs stay < 2e8 per i32 lane). */
#define I16_SCALE         5000
#define I16_SENTINEL      (-5000)

/* KD-tree (format v13). Node = 72 bytes: bmin[16]i16 bmax[16]i16 a(u32) b(u32).
 * Leaf iff (b & KD_LEAF_FLAG): a = first record idx, count = b & KD_COUNT_MASK.
 * Internal: a = left child idx, b = right child idx (< 2^31). */
#define LEAF_SIZE         32
#define KD_NODE_BYTES     72
#define KD_LEAF_FLAG      0x80000000u
#define KD_COUNT_MASK     0x7fffffffu
/* Traversal stack depth: ample headroom over the builder's reported max depth. */
#define KD_STACK_SIZE     192

#define HTTP_PORT_DEFAULT 8080
#define MAX_CONNECTIONS   256
#define READ_BUF_SIZE     4096
#define MAX_BODY_SIZE     4096

#endif /* COMPAT_H */

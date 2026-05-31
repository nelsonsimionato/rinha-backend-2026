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
#define RECORD_STRIDE     16
#define K                 5
#define FORMAT_VERSION    12          /* v12: balanced KD-tree (Phase 1) */
#define HEADER_SIZE       16

/* KD-tree (format v12). Node = 40 bytes: bmin[16] bmax[16] a(u32) b(u32).
 * Leaf iff (b & KD_LEAF_FLAG): a = first record idx, count = b & KD_COUNT_MASK.
 * Internal: a = left child idx, b = right child idx (< 2^31). */
#define LEAF_SIZE         32
#define KD_NODE_BYTES     40
#define KD_LEAF_FLAG      0x80000000u
#define KD_COUNT_MASK     0x7fffffffu
/* Traversal stack depth: ample headroom over the builder's reported max depth. */
#define KD_STACK_SIZE     192

#define HTTP_PORT_DEFAULT 8080
#define MAX_CONNECTIONS   256
#define READ_BUF_SIZE     4096
#define MAX_BODY_SIZE     4096

#endif /* COMPAT_H */

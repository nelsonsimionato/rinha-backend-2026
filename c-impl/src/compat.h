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

/* Match Go v0.12 constants byte-for-byte. */
#define DIMENSIONS        14
#define RECORD_STRIDE     16
#define K                 5
#define PARTITION_COUNT   16384
#define FORMAT_VERSION    11
#define HEADER_SIZE       16
#define MAX_CANDIDATES    64
#define SKIP_BOUND_PRUNE_IF_MATCHING_AT_LEAST 100

#define HTTP_PORT_DEFAULT 8080
#define MAX_CONNECTIONS   256
#define READ_BUF_SIZE     4096
#define MAX_BODY_SIZE     4096

#endif /* COMPAT_H */

#ifndef INDEX_H
#define INDEX_H

#include "compat.h"

/* Global pointers into the mmap'd index file. Read-only after init.
 * Layout per format v11 (matches tools/build_partition_hash.go):
 *   [0:16]     header
 *   [16: ...]                   partitionOffsets[PARTITION_COUNT] uint32
 *   [+]                         partitionSizes[PARTITION_COUNT]   uint32
 *   [+]                         partitionMin   [PARTITION_COUNT][16] uint8
 *   [+]                         partitionMax   [PARTITION_COUNT][16] uint8
 *   [+]                         data[N][16] uint8 (sorted by partition)
 *   [+]                         isFraud[N]  uint8
 */
extern const uint32_t *partition_offsets;
extern const uint32_t *partition_sizes;
extern const uint8_t  *partition_min;    /* PARTITION_COUNT * 16 bytes */
extern const uint8_t  *partition_max;
extern const uint8_t  *data;             /* total * 16 bytes */
extern const uint8_t  *is_fraud;         /* total bytes */
extern uint32_t        total_records;

/* Pre-computed non-empty partition indices for fast iteration in SearchKNN. */
extern uint16_t        non_empty_partitions[PARTITION_COUNT];
extern uint32_t        non_empty_count;

/* mmap the index file, validate format version, set up all pointers and the
 * non_empty_partitions[] list. Returns 0 on success, -1 on failure. */
int index_load(const char *path);

#endif /* INDEX_H */

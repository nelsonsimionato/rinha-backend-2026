#ifndef INDEX_H
#define INDEX_H

#include "compat.h"

/* KD-tree node, 40 bytes. Mirrors tools/build_kdtree.go on-disk layout.
 *   bmin/bmax : subtree axis-aligned bounding box (dims 14,15 = 0).
 *   a, b      : internal -> (left child idx, right child idx, b high bit clear)
 *               leaf     -> (first record idx, count | KD_LEAF_FLAG)
 */
typedef struct {
	uint8_t  bmin[RECORD_STRIDE];
	uint8_t  bmax[RECORD_STRIDE];
	uint32_t a;
	uint32_t b;
} KDNode;

/* Global pointers into the mmap'd index file. Read-only after init.
 * Layout per format v12 (matches tools/build_kdtree.go):
 *   [0:16]   header (version, N at [4:8], nodeCount at [8:12])
 *   [16:..]  nodes[node_count] × KDNode (40 bytes)
 *   [..]     data[N][16] uint8 (reordered into KD leaf order)
 *   [..]     isFraud[N]  uint8 (same order)
 */
extern const KDNode  *kd_nodes;
extern uint32_t       kd_node_count;
extern const uint8_t *data;            /* total * 16 bytes, leaf-ordered */
extern const uint8_t *is_fraud;        /* total bytes, same order */
extern uint32_t       total_records;

/* mmap the index file, validate format version, set up all pointers.
 * Returns 0 on success, -1 on failure. */
int index_load(const char *path);

#endif /* INDEX_H */

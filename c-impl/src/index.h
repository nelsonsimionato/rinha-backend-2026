#ifndef INDEX_H
#define INDEX_H

#include "compat.h"

/* KD-tree node, 72 bytes. Mirrors tools/build_kdtree.go on-disk layout.
 *   bmin/bmax : subtree axis-aligned bounding box, 16 int16 lanes (14,15 = 0).
 *   a, b      : internal -> (left child idx, right child idx, b high bit clear)
 *               leaf     -> (first record idx, count | KD_LEAF_FLAG)
 */
typedef struct {
	int16_t  bmin[RECORD_STRIDE];
	int16_t  bmax[RECORD_STRIDE];
	uint32_t a;
	uint32_t b;
} KDNode;

/* Global pointers into the mmap'd index file. Read-only after init.
 * Layout per format v13 (matches tools/build_kdtree.go):
 *   [0:16]   header (version, N at [4:8], nodeCount at [8:12])
 *   [16:..]  nodes[node_count] × KDNode (72 bytes)
 *   [..]     data[N][16] int16 (reordered into KD leaf order)
 *   [..]     isFraud[N]  uint8 (same order)
 */
extern const KDNode  *kd_nodes;
extern uint32_t       kd_node_count;
extern const int16_t *data;            /* total * 16 int16 lanes, leaf-ordered */
extern const uint8_t *is_fraud;        /* total bytes, same order */
extern uint32_t       total_records;

/* mmap the index file, validate format version, set up all pointers.
 * Returns 0 on success, -1 on failure. */
int index_load(const char *path);

#endif /* INDEX_H */

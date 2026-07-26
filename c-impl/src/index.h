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

/* ---- SEARCH_V2 (env-gated, default off): Haswell-tuned exact-search layout.
 * Built at load time from the v14 file; the file format is unchanged.
 *
 * LeafChunk: 8 records in dimension-pair-interleaved SoA. pairs[p] holds lanes
 * (2p, 2p+1) for all 8 records: [r0.d2p, r0.d2p+1, r1.d2p, r1.d2p+1, ...].
 * Only pairs 0..6 (lanes 0..13) are stored — lanes 14,15 are 0 on both query
 * (vectorize memset) and records (builder pad), contributing nothing. One
 * 32-byte load per pair covers all 8 records; vpmaddwd accumulates distances
 * vertically with no per-record horizontal reduction.
 *
 * NodeBounds/NodeKids: the 72-byte KDNode split so the pruning path touches
 * exactly one 64-byte line per node (bmin+bmax), children a separate 8B. For
 * leaves, NodeKids.a = LeafDesc index (NOT the record index; that moves into
 * LeafDesc.rec_base), b keeps KD_LEAF_FLAG. */
typedef struct {
	int16_t pairs[7][16];
} LeafChunk;                            /* 224 bytes */

typedef struct {
	uint32_t chunk_first;               /* index into g_leaf_chunks */
	uint32_t rec_base;                  /* first record idx (leaf order) */
	uint32_t count;                     /* records in this leaf */
} LeafDesc;

typedef struct {
	int16_t bmin[RECORD_STRIDE];
	int16_t bmax[RECORD_STRIDE];
} NodeBounds;                           /* 64 bytes = one cache line */

typedef struct {
	uint32_t a, b;
} NodeKids;                             /* 8 bytes */

STATIC_ASSERT(sizeof(LeafChunk) == 224, "LeafChunk must be 224 bytes");
STATIC_ASSERT(sizeof(NodeBounds) == 64, "NodeBounds must be one cache line");

extern const LeafChunk  *g_leaf_chunks;
extern const LeafDesc   *g_leaf_desc;
extern const NodeBounds *g_node_bounds;
extern const NodeKids   *g_node_kids;
extern int               g_search_v2;   /* 1 = v2 arrays built and active */

/* mmap the index file, validate format version, set up all pointers.
 * Returns 0 on success, -1 on failure. */
int index_load(const char *path);

#endif /* INDEX_H */

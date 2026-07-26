#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "index.h"

const KDNode  *kd_nodes;
uint32_t       kd_node_count;
const int16_t *data;
const uint8_t *is_fraud;
uint32_t       total_records;

/* SEARCH_V2 arrays (anon, built by build_v2_arrays when SEARCH_V2=1). */
const LeafChunk  *g_leaf_chunks;
const LeafDesc   *g_leaf_desc;
const NodeBounds *g_node_bounds;
const NodeKids   *g_node_kids;
int               g_search_v2;

/* dummy byte to force page reads (kept volatile so the compiler doesn't
 * eliminate the prewarm loop). */
static volatile uint8_t prewarm_sink;

static uint32_t rd_u32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
	       (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* Anonymous page-aligned allocation (page alignment >= the 64B we need). */
static void *anon_alloc(size_t bytes)
{
	void *p = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
	               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	return p == MAP_FAILED ? NULL : p;
}

/* Build the SEARCH_V2 arrays (leaf chunks in dimension-pair SoA + split node
 * arrays) from the freshly mapped v14 file. Streaming single pass; on ANY
 * failure everything is released and v1 serving continues unchanged.
 * Returns 0 and sets g_search_v2=1 on success. */
static int build_v2_arrays(void)
{
	/* Pass A: count leaves and chunks. */
	uint32_t leaf_count = 0;
	uint64_t chunk_count = 0;
	for (uint32_t ni = 0; ni < kd_node_count; ni++) {
		if (kd_nodes[ni].b & KD_LEAF_FLAG) {
			uint32_t cnt = kd_nodes[ni].b & KD_COUNT_MASK;
			leaf_count++;
			chunk_count += (cnt + 7u) >> 3;
		}
	}
	if (leaf_count == 0 || chunk_count == 0) return -1;

	size_t sz_bounds = (size_t)kd_node_count * sizeof(NodeBounds);
	size_t sz_kids   = (size_t)kd_node_count * sizeof(NodeKids);
	size_t sz_desc   = (size_t)leaf_count    * sizeof(LeafDesc);
	size_t sz_chunks = (size_t)chunk_count   * sizeof(LeafChunk);

	NodeBounds *bounds = anon_alloc(sz_bounds);
	NodeKids   *kids   = anon_alloc(sz_kids);
	LeafDesc   *desc   = anon_alloc(sz_desc);
	LeafChunk  *chunks = anon_alloc(sz_chunks);
	if (!bounds || !kids || !desc || !chunks) {
		if (bounds) munmap(bounds, sz_bounds);
		if (kids)   munmap(kids, sz_kids);
		if (desc)   munmap(desc, sz_desc);
		if (chunks) munmap(chunks, sz_chunks);
		return -1;
	}

	/* Pass B: split nodes; per leaf, emit 8-record SoA chunks. */
	uint32_t leaf_idx = 0;
	uint32_t chunk_cursor = 0;
	for (uint32_t ni = 0; ni < kd_node_count; ni++) {
		const KDNode *nd = &kd_nodes[ni];
		memcpy(bounds[ni].bmin, nd->bmin, sizeof(bounds[ni].bmin));
		memcpy(bounds[ni].bmax, nd->bmax, sizeof(bounds[ni].bmax));
		if (!(nd->b & KD_LEAF_FLAG)) {
			kids[ni].a = nd->a;
			kids[ni].b = nd->b;
			continue;
		}
		uint32_t base = nd->a;
		uint32_t cnt  = nd->b & KD_COUNT_MASK;
		uint32_t nch  = (cnt + 7u) >> 3;
		kids[ni].a = leaf_idx;
		kids[ni].b = KD_LEAF_FLAG;          /* count lives in LeafDesc */
		desc[leaf_idx].chunk_first = chunk_cursor;
		desc[leaf_idx].rec_base    = base;
		desc[leaf_idx].count       = cnt;
		for (uint32_t c = 0; c < nch; c++) {
			LeafChunk *ch = &chunks[chunk_cursor + c];
			for (uint32_t p = 0; p < 7; p++) {
				for (uint32_t j = 0; j < 8; j++) {
					uint32_t rec = base + c * 8u + j;
					if (rec < base + cnt) {
						const int16_t *rv = &data[(size_t)rec * RECORD_STRIDE];
						ch->pairs[p][2*j]     = rv[2*p];
						ch->pairs[p][2*j + 1] = rv[2*p + 1];
					} else {
						ch->pairs[p][2*j]     = 0;  /* pad: never pushed */
						ch->pairs[p][2*j + 1] = 0;
					}
				}
			}
		}
		chunk_cursor += nch;
		leaf_idx++;
	}

	g_node_bounds = bounds;
	g_node_kids   = kids;
	g_leaf_desc   = desc;
	g_leaf_chunks = chunks;
	g_search_v2   = 1;
	fprintf(stderr,
	        "index_load: SEARCH_V2 arrays built: %u leaves, %llu chunks, %.1f MB\n",
	        leaf_count, (unsigned long long)chunk_count,
	        (double)(sz_bounds + sz_kids + sz_desc + sz_chunks) / 1e6);
	return 0;
}

int index_load(const char *path)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		fprintf(stderr, "index_load: open %s failed\n", path);
		return -1;
	}
	struct stat st;
	if (fstat(fd, &st) < 0) {
		fprintf(stderr, "index_load: fstat failed\n");
		close(fd);
		return -1;
	}
	size_t size = (size_t)st.st_size;
	void *p = mmap(NULL, size, PROT_READ, MAP_PRIVATE | MAP_POPULATE, fd, 0);
	close(fd);
	if (p == MAP_FAILED) {
		fprintf(stderr, "index_load: mmap failed\n");
		return -1;
	}

	/* INDEX_HUGE=1: copy the index into an anonymous MADV_HUGEPAGE region.
	 * File-backed mappings cannot use THP; an anon copy lets the kernel back
	 * the 118MB working set with 2MB pages instead of ~29k 4KB pages —
	 * collapsing dTLB misses + page walks on every random KD-tree descent.
	 * Needs host THP mode "madvise" or "always" (Ubuntu default: madvise);
	 * degrades to the plain file mapping on any failure. */
	const char *eh = getenv("INDEX_HUGE");
	if (eh && eh[0] == '1') {
		size_t align = 2UL << 20;
		size_t sz2m  = (size + align - 1) & ~(align - 1);
		void *raw = mmap(NULL, sz2m + align, PROT_READ | PROT_WRITE,
		                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (raw != MAP_FAILED) {
			uintptr_t base    = ((uintptr_t)raw + align - 1) & ~(uintptr_t)(align - 1);
			size_t    headcut = (size_t)(base - (uintptr_t)raw);
			if (headcut)                  munmap(raw, headcut);
			if (align - headcut)          munmap((void *)(base + sz2m), align - headcut);
			void *anon = (void *)base;
			if (madvise(anon, sz2m, MADV_HUGEPAGE) != 0)
				fprintf(stderr, "index_load: MADV_HUGEPAGE unavailable (%s), copying anyway\n",
				        strerror(errno));
			memcpy(anon, p, size);
			if (mprotect(anon, sz2m, PROT_READ) != 0) { /* keep read-only discipline */ }
			munmap(p, size);
			p = anon;
			fprintf(stderr, "index_load: index copied to anon region (THP requested)\n");
		} else {
			fprintf(stderr, "index_load: anon alloc failed (%s), using file mapping\n",
			        strerror(errno));
		}
	}

	/* INDEX_MLOCK=1: pin the index so clean pages can never be reclaimed and
	 * refaulted mid-request (tail spikes). Requires RLIMIT_MEMLOCK raised via
	 * compose ulimits (memlock: -1); logged no-op otherwise. */
	const char *em = getenv("INDEX_MLOCK");
	if (em && em[0] == '1') {
		if (mlock(p, size) == 0)
			fprintf(stderr, "index_load: mlock(%zu MB) ok\n", size >> 20);
		else
			fprintf(stderr, "index_load: mlock failed (%s), continuing unlocked\n",
			        strerror(errno));
	}

	const uint8_t *buf = (const uint8_t *)p;
	if (size < HEADER_SIZE) {
		fprintf(stderr, "index_load: file too small (%zu bytes)\n", size);
		return -1;
	}
	uint8_t ver = buf[0];
	if (ver != FORMAT_VERSION) {
		fprintf(stderr, "index_load: format version %u, expected %u\n",
		        (unsigned)ver, (unsigned)FORMAT_VERSION);
		return -1;
	}
	uint32_t total  = rd_u32(buf + 4);   /* N         */
	uint32_t nnodes = rd_u32(buf + 8);   /* nodeCount */
	total_records = total;
	kd_node_count = nnodes;

	size_t off = HEADER_SIZE;
	kd_nodes = (const KDNode *)(buf + off);
	off += (size_t)nnodes * KD_NODE_BYTES;

	size_t data_len = (size_t)total * RECORD_STRIDE * sizeof(int16_t);
	size_t expected = off + data_len + total;
	if (size < expected) {
		fprintf(stderr, "index_load: truncated (%zu bytes, expected %zu)\n", size, expected);
		return -1;
	}
	data     = (const int16_t *)(buf + off);
	off     += data_len;
	is_fraud = buf + off;

	/* SEARCH_V2=1: build the Haswell-tuned exact-search arrays (leaf-chunk
	 * SoA + split nodes). The transform writes every byte it allocates, so
	 * the new arrays are already faulted in; is_fraud stays file-backed. */
	const char *sv2 = getenv("SEARCH_V2");
	if (sv2 && sv2[0] == '1') {
		if (build_v2_arrays() != 0)
			fprintf(stderr, "index_load: SEARCH_V2 build failed, serving v1 path\n");
	}

	/* Pre-warm physical pages: 1 byte per 4 KB page faults each one in. */
	for (size_t i = 0; i < size; i += 4096) {
		prewarm_sink += buf[i];
	}

	fprintf(stderr,
	        "index_load: v%u, records=%u, kd_nodes=%u, %.1f MB\n",
	        (unsigned)ver, total, nnodes, (double)size / 1e6);
	return 0;
}

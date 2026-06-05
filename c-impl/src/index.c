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

/* dummy byte to force page reads (kept volatile so the compiler doesn't
 * eliminate the prewarm loop). */
static volatile uint8_t prewarm_sink;

static uint32_t rd_u32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
	       (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
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

	/* Pre-warm physical pages: 1 byte per 4 KB page faults each one in. */
	for (size_t i = 0; i < size; i += 4096) {
		prewarm_sink += buf[i];
	}

	fprintf(stderr,
	        "index_load: v%u, records=%u, kd_nodes=%u, %.1f MB\n",
	        (unsigned)ver, total, nnodes, (double)size / 1e6);
	return 0;
}

#include <fcntl.h>
#include <stdio.h>
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

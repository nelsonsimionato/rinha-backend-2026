#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "index.h"

const uint32_t *partition_offsets;
const uint32_t *partition_sizes;
const uint8_t  *partition_min;
const uint8_t  *partition_max;
const uint8_t  *data;
const uint8_t  *is_fraud;
uint32_t        total_records;

uint16_t        non_empty_partitions[PARTITION_COUNT];
uint32_t        non_empty_count;

/* dummy byte to force page reads (kept volatile so the compiler doesn't
 * eliminate the prewarm loop). */
static volatile uint8_t prewarm_sink;

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
	/* totalRecords stored at offset 4 (little-endian uint32) */
	uint32_t total =
	      ((uint32_t)buf[4])        |
	      ((uint32_t)buf[5]) <<  8  |
	      ((uint32_t)buf[6]) << 16  |
	      ((uint32_t)buf[7]) << 24;
	total_records = total;

	size_t off = HEADER_SIZE;
	partition_offsets = (const uint32_t *)(buf + off);
	off += PARTITION_COUNT * 4;
	partition_sizes   = (const uint32_t *)(buf + off);
	off += PARTITION_COUNT * 4;
	partition_min     = buf + off;
	off += PARTITION_COUNT * RECORD_STRIDE;
	partition_max     = buf + off;
	off += PARTITION_COUNT * RECORD_STRIDE;

	size_t data_len    = (size_t)total * RECORD_STRIDE;
	size_t expected    = off + data_len + total;
	if (size < expected) {
		fprintf(stderr, "index_load: truncated (%zu bytes, expected %zu)\n", size, expected);
		return -1;
	}
	data     = buf + off;
	off     += data_len;
	is_fraud = buf + off;

	/* Build non_empty_partitions[] for fast iteration */
	non_empty_count = 0;
	for (uint32_t i = 0; i < PARTITION_COUNT; i++) {
		if (partition_sizes[i] > 0) {
			non_empty_partitions[non_empty_count++] = (uint16_t)i;
		}
	}

	/* Pre-warm physical pages: 1 byte per 4 KB page faults each one in. */
	for (size_t i = 0; i < size; i += 4096) {
		prewarm_sink += buf[i];
	}

	fprintf(stderr,
	        "index_load: v%u, records=%u, non_empty=%u/%u, %.1f MB\n",
	        (unsigned)ver, total, non_empty_count, PARTITION_COUNT,
	        (double)size / 1e6);
	return 0;
}

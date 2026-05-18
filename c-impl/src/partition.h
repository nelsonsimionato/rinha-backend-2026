#ifndef PARTITION_H
#define PARTITION_H

#include "compat.h"

/* 14-bit feature-hash partition key for a quantized 16-byte vector.
 * MUST match the encoding in tools/build_partition_hash.go and
 * the Go runtime's partitionKey in main.go bit-for-bit. */
ALWAYS_INLINE uint16_t partition_key(const uint8_t v[RECORD_STRIDE])
{
	uint16_t k = 0;
	if (v[5] == 0 && v[6] == 0) k |= 1u << 0;
	if (v[9]  > 128)             k |= 1u << 1;
	if (v[10] > 128)             k |= 1u << 2;
	if (v[11] > 128)             k |= 1u << 3;
	k |= (uint16_t)(v[12] >> 6) << 4;
	k |= (uint16_t)(v[2]  >> 6) << 6;
	if (v[7]  > 128)             k |= 1u << 8;
	if (v[8]  > 128)             k |= 1u << 9;
	if (v[0]  > 128)             k |= 1u << 10;
	if (v[3]  > 128)             k |= 1u << 11;
	if (v[4]  > 128)             k |= 1u << 12;
	if (v[13] > 128)             k |= 1u << 13;
	return k;
}

#endif /* PARTITION_H */

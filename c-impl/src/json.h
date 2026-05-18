#ifndef JSON_H
#define JSON_H

#include "compat.h"
#include "vectorize.h"

/* Parse a fraud-score POST body into a Payload. The Payload's string fields
 * (timestamps, IDs, known_merchants entries) are slices into `buf` — no copy.
 * Caller must keep `buf` alive while using `p`.
 *
 * Returns 0 on success, -1 on any parse error. Callers translate -1 into a
 * safe fallback HTTP 200 response (never 4xx/5xx). */
int json_parse(const char *buf, int len, Payload *p);

#endif /* JSON_H */

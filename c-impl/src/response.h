#ifndef RESPONSE_H
#define RESPONSE_H

#include "compat.h"

/* Pre-formatted HTTP responses, indexed by fraud-count in [0..K].
 * - responses[0] is the safe fallback ("approved":true, fraud_score 0.0000)
 *   used whenever parsing or processing fails.
 * - 6 entries total: 0,1,2 → approved:true; 3,4,5 → approved:false.
 *
 * Bodies and headers are baked into the same buffer so we issue a single
 * write() per response. */

extern const char *const HTTP_RESPONSES[K + 1];
extern int               HTTP_RESPONSES_LEN[K + 1];

extern const char *const HTTP_READY_RESP;
extern int               HTTP_READY_RESP_LEN;

extern const char *const HTTP_NOT_FOUND_RESP;
extern int               HTTP_NOT_FOUND_RESP_LEN;

#endif /* RESPONSE_H */

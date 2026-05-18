#include "response.h"

/* IMPORTANT: bodies vary by one byte (true=4 chars, false=5 chars) so each
 * response needs its own Content-Length. Mismatched lengths under HTTP/1.1
 * keep-alive desync the connection — the extra byte flows into the next
 * request stream, after which every subsequent request on that connection
 * fails to parse and returns the safe fallback. This was the v0.13 first-
 * push bug that capped score at 819.73 (true_positive_detections: 0). */

/* "true"  → 38 bytes  /  "false" → 39 bytes */
#define RESP_38(body) \
	"HTTP/1.1 200 OK\r\n"                       \
	"Content-Type: application/json\r\n"        \
	"Content-Length: 38\r\n"                    \
	"Connection: keep-alive\r\n"                \
	"\r\n"                                       \
	body

#define RESP_39(body) \
	"HTTP/1.1 200 OK\r\n"                       \
	"Content-Type: application/json\r\n"        \
	"Content-Length: 39\r\n"                    \
	"Connection: keep-alive\r\n"                \
	"\r\n"                                       \
	body

static const char R0[] = RESP_38("{\"approved\":true,\"fraud_score\":0.0000}");
static const char R1[] = RESP_38("{\"approved\":true,\"fraud_score\":0.2000}");
static const char R2[] = RESP_38("{\"approved\":true,\"fraud_score\":0.4000}");
static const char R3[] = RESP_39("{\"approved\":false,\"fraud_score\":0.6000}");
static const char R4[] = RESP_39("{\"approved\":false,\"fraud_score\":0.8000}");
static const char R5[] = RESP_39("{\"approved\":false,\"fraud_score\":1.0000}");

const char *const HTTP_RESPONSES[K + 1] = { R0, R1, R2, R3, R4, R5 };
int HTTP_RESPONSES_LEN[K + 1] = {
	(int)sizeof(R0) - 1,
	(int)sizeof(R1) - 1,
	(int)sizeof(R2) - 1,
	(int)sizeof(R3) - 1,
	(int)sizeof(R4) - 1,
	(int)sizeof(R5) - 1,
};

static const char READY_RESP[] =
	"HTTP/1.1 200 OK\r\n"
	"Content-Length: 0\r\n"
	"Connection: keep-alive\r\n"
	"\r\n";

static const char NOT_FOUND_RESP[] =
	"HTTP/1.1 404 Not Found\r\n"
	"Content-Length: 0\r\n"
	"Connection: keep-alive\r\n"
	"\r\n";

const char *const HTTP_READY_RESP = READY_RESP;
int HTTP_READY_RESP_LEN           = (int)sizeof(READY_RESP) - 1;

const char *const HTTP_NOT_FOUND_RESP = NOT_FOUND_RESP;
int HTTP_NOT_FOUND_RESP_LEN           = (int)sizeof(NOT_FOUND_RESP) - 1;

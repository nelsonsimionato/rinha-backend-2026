#ifndef HTTP_H
#define HTTP_H

#include "compat.h"

typedef enum {
	HTTP_NONE          = 0,
	HTTP_GET_READY     = 1,
	HTTP_POST_FRAUD    = 2,
	HTTP_OTHER         = 3,  /* unknown method/path */
} HttpRoute;

typedef struct {
	HttpRoute   route;
	int         content_length;
	int         headers_len;     /* bytes from buf[0] to (including) "\r\n\r\n" */
	const char *body;            /* pointer into buf, NULL until body fully received */
	int         body_len;        /* set when body is available */
} HttpRequest;

/* Parses HTTP request line + headers in `buf[0..len)`.
 * Returns:
 *   0  → not enough data yet (need more bytes)
 *   1  → headers parsed (req->headers_len set; check route + content_length)
 *  -1  → malformed (caller emits safe fallback response and closes)
 *
 * For POST routes, caller must wait until `len >= headers_len + content_length`
 * before calling http_set_body().
 */
int http_parse_headers(const char *buf, int len, HttpRequest *req);

ALWAYS_INLINE void http_set_body(HttpRequest *req, const char *buf)
{
	req->body     = buf + req->headers_len;
	req->body_len = req->content_length;
}

#endif /* HTTP_H */

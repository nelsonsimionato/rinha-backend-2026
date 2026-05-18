/* Minimal HTTP/1.1 parser for two endpoints: GET /ready and POST /fraud-score.
 *
 * No regex, no allocation. Recognizes Content-Length case-insensitively.
 * Anything else (other methods, paths, malformed) returns HTTP_OTHER or -1.
 */

#include <string.h>
#include "http.h"

static int ichar(int c) { return (c >= 'A' && c <= 'Z') ? (c | 0x20) : c; }

static int parse_content_length(const char *p, int n, int *out)
{
	if (n <= 0) return -1;
	int v = 0;
	int i = 0;
	while (i < n && (p[i] == ' ' || p[i] == '\t')) i++;
	if (i == n) return -1;
	int seen = 0;
	while (i < n && p[i] >= '0' && p[i] <= '9') {
		v = v * 10 + (p[i] - '0');
		i++; seen = 1;
		if (v > MAX_BODY_SIZE) return -1;
	}
	if (!seen) return -1;
	*out = v;
	return 0;
}

int http_parse_headers(const char *buf, int len, HttpRequest *req)
{
	req->route          = HTTP_NONE;
	req->content_length = 0;
	req->headers_len    = 0;
	req->body           = 0;
	req->body_len       = 0;

	/* request line — minimum "GET / HTTP/1.1\r\n" = 16 bytes */
	if (len < 16) return 0;

	/* method + path detection. We only care about two specific tuples. */
	if (memcmp(buf, "GET /ready ", 11) == 0) {
		req->route = HTTP_GET_READY;
	} else if (memcmp(buf, "POST /fraud-score ", 18) == 0) {
		req->route = HTTP_POST_FRAUD;
	} else {
		/* Could still be valid HTTP we don't serve. Walk to end of headers. */
		req->route = HTTP_OTHER;
	}

	/* Find end of request line (\r\n) */
	int i = 0;
	while (i + 1 < len && !(buf[i] == '\r' && buf[i+1] == '\n')) i++;
	if (i + 1 >= len) return 0; /* need more */
	i += 2;

	/* Walk headers; look for "\r\n\r\n" or "Content-Length:" */
	while (i < len) {
		/* End of headers: empty line "\r\n" */
		if (i + 1 < len && buf[i] == '\r' && buf[i+1] == '\n') {
			req->headers_len = i + 2;
			return 1;
		}
		/* Find end of this header line */
		int line_start = i;
		while (i + 1 < len && !(buf[i] == '\r' && buf[i+1] == '\n')) i++;
		if (i + 1 >= len) return 0;

		/* Check for Content-Length (case-insensitive) */
		int line_len = i - line_start;
		if (line_len >= 16) {
			static const char CL[] = "Content-Length:";
			int match = 1;
			for (int k = 0; k < (int)sizeof(CL) - 1; k++) {
				if (ichar(buf[line_start + k]) != ichar(CL[k])) {
					match = 0; break;
				}
			}
			if (match) {
				const char *v   = buf + line_start + (int)sizeof(CL) - 1;
				int         vlen = line_len - ((int)sizeof(CL) - 1);
				if (parse_content_length(v, vlen, &req->content_length) < 0) return -1;
			}
		}
		i += 2; /* skip \r\n */
	}
	return 0; /* incomplete */
}

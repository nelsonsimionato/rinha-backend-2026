#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "compat.h"
#include "index.h"
#include "server.h"

static const char *DEFAULT_INDEX_PATH = "/resources/index.bin";

/* Used by Docker healthcheck. Connects to the local listener and probes
 * /ready. Exits 0 on a 200 response; non-zero otherwise. Self-contained
 * so the scratch image needs no extra binary. */
static int run_healthcheck(int port)
{
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) return 2;
	struct sockaddr_in addr = {0};
	addr.sin_family      = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port        = htons((uint16_t)port);
	if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
		close(fd); return 3;
	}
	static const char req[] = "GET /ready HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
	if (send(fd, req, sizeof(req) - 1, 0) < 0) { close(fd); return 4; }
	char buf[64];
	int n = (int)recv(fd, buf, sizeof(buf) - 1, 0);
	close(fd);
	if (n < 12) return 5;
	buf[n] = 0;
	return memcmp(buf, "HTTP/1.1 200", 12) == 0 ? 0 : 6;
}

int main(int argc, char **argv)
{
	const char *index_path = DEFAULT_INDEX_PATH;
	int         port       = HTTP_PORT_DEFAULT;
	int         hc_mode    = 0;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--index") == 0 && i + 1 < argc) {
			index_path = argv[++i];
		} else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
			port = atoi(argv[++i]);
		} else if (strcmp(argv[i], "--healthcheck") == 0) {
			hc_mode = 1;
		}
	}
	const char *env_index = getenv("INDEX_PATH");
	if (env_index && env_index[0]) index_path = env_index;
	const char *env_port = getenv("PORT");
	if (env_port && env_port[0]) port = atoi(env_port);

	if (hc_mode) return run_healthcheck(port);

	if (index_load(index_path) != 0) {
		fprintf(stderr, "main: failed to load index from %s\n", index_path);
		return 1;
	}

	return server_run(port);
}

/* Minimal TCP load balancer.
 *
 * - epoll edge-triggered, single thread.
 * - Round-robin across backends supplied on the command line:
 *     ./lb --port 9999 api1:8080 api2:8080
 * - Per connection-pair: one client fd, one backend fd, two pipes for
 *   zero-copy splice() in each direction.
 * - Pure byte forwarder. Does not parse HTTP; clients negotiate keep-alive
 *   directly with the backend.
 * - No allocation on hot path; fixed slot pool (1024 pairs).
 *
 * The goal is to use far less CPU than HAProxy by avoiding userspace copy.
 */

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#define MAX_PAIRS    1024
#define MAX_BACKENDS 16
#define SPLICE_CHUNK 16384  /* bytes per splice() call; 16K = comfortable */

typedef struct {
	int  client_fd;       /* -1 = slot free */
	int  backend_fd;
	int  c_to_b_pipe[2];  /* splice client -> pipe -> backend */
	int  b_to_c_pipe[2];  /* splice backend -> pipe -> client */
	int  c_to_b_inflight;
	int  b_to_c_inflight;
	int  closed;
} Pair;

typedef struct {
	struct sockaddr_in addr;
	char               host[64];
} Backend;

static Pair    pairs[MAX_PAIRS];
static Backend backends[MAX_BACKENDS];
static int     n_backends = 0;
static int     rr_next    = 0;
static int     epfd       = -1;
static int     listen_fd  = -1;
static volatile sig_atomic_t stop_flag = 0;

static void on_signal(int s) { (void)s; stop_flag = 1; }

static int find_free_pair(void)
{
	for (int i = 0; i < MAX_PAIRS; i++)
		if (pairs[i].client_fd < 0) return i;
	return -1;
}

static int pair_index_for_fd(int fd, int *is_client)
{
	for (int i = 0; i < MAX_PAIRS; i++) {
		if (pairs[i].client_fd == fd) { *is_client = 1; return i; }
		if (pairs[i].backend_fd == fd) { *is_client = 0; return i; }
	}
	return -1;
}

static void pair_close(Pair *p)
{
	if (p->closed) return;
	p->closed = 1;
	if (p->client_fd  >= 0) { epoll_ctl(epfd, EPOLL_CTL_DEL, p->client_fd,  NULL); close(p->client_fd); }
	if (p->backend_fd >= 0) { epoll_ctl(epfd, EPOLL_CTL_DEL, p->backend_fd, NULL); close(p->backend_fd); }
	if (p->c_to_b_pipe[0] >= 0) close(p->c_to_b_pipe[0]);
	if (p->c_to_b_pipe[1] >= 0) close(p->c_to_b_pipe[1]);
	if (p->b_to_c_pipe[0] >= 0) close(p->b_to_c_pipe[0]);
	if (p->b_to_c_pipe[1] >= 0) close(p->b_to_c_pipe[1]);
	p->client_fd        = -1;
	p->backend_fd       = -1;
	p->c_to_b_pipe[0]   = -1;
	p->c_to_b_pipe[1]   = -1;
	p->b_to_c_pipe[0]   = -1;
	p->b_to_c_pipe[1]   = -1;
	p->c_to_b_inflight  = 0;
	p->b_to_c_inflight  = 0;
}

/* Drain pipe inflight (>=0). Returns 1 if more data, 0 if pipe empty, -1 fatal. */
static int drain_pipe_to(int pipe_r, int dst_fd, int *inflight)
{
	while (*inflight > 0) {
		ssize_t n = splice(pipe_r, NULL, dst_fd, NULL, *inflight,
		                   SPLICE_F_MOVE | SPLICE_F_NONBLOCK);
		if (n > 0) { *inflight -= (int)n; continue; }
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 1;
		return -1;
	}
	return 0;
}

/* Forward one direction: src_fd → pipe → dst_fd. */
static int forward(int src_fd, int pipe_w, int pipe_r, int dst_fd, int *inflight)
{
	if (drain_pipe_to(pipe_r, dst_fd, inflight) < 0) return -1;
	for (;;) {
		ssize_t n = splice(src_fd, NULL, pipe_w, NULL, SPLICE_CHUNK,
		                   SPLICE_F_MOVE | SPLICE_F_NONBLOCK);
		if (n > 0) {
			*inflight += (int)n;
			if (drain_pipe_to(pipe_r, dst_fd, inflight) < 0) return -1;
			continue;
		}
		if (n == 0) return -2;
		if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
		return -1;
	}
}

static int set_nonblock(int fd)
{
	int fl = fcntl(fd, F_GETFL, 0);
	if (fl < 0) return -1;
	return fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

static int connect_backend_nb(int idx)
{
	int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (fd < 0) return -1;
	int one = 1;
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
	int rc = connect(fd, (struct sockaddr*)&backends[idx].addr, sizeof(struct sockaddr_in));
	if (rc < 0 && errno != EINPROGRESS) { close(fd); return -1; }
	return fd;
}

static int resolve_backend(const char *spec, Backend *bk)
{
	char host[64]; int port = 0;
	const char *colon = strchr(spec, ':');
	if (!colon) return -1;
	size_t hl = (size_t)(colon - spec);
	if (hl >= sizeof(host)) return -1;
	memcpy(host, spec, hl); host[hl] = 0;
	port = atoi(colon + 1);
	if (port <= 0) return -1;

	struct addrinfo hints = {0}, *res = NULL;
	hints.ai_family   = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	int rc = getaddrinfo(host, NULL, &hints, &res);
	if (rc != 0 || !res) {
		fprintf(stderr, "lb: resolve %s: %s\n", host, gai_strerror(rc));
		return -1;
	}
	memcpy(&bk->addr, res->ai_addr, sizeof(struct sockaddr_in));
	bk->addr.sin_port = htons((uint16_t)port);
	snprintf(bk->host, sizeof(bk->host), "%s:%d", host, port);
	freeaddrinfo(res);
	return 0;
}

static int setup_listener(int port)
{
	int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (fd < 0) return -1;
	int one = 1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
	struct sockaddr_in addr = {0};
	addr.sin_family      = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port        = htons((uint16_t)port);
	if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) { close(fd); return -1; }
	if (listen(fd, 1024) < 0) { close(fd); return -1; }
	return fd;
}

int main(int argc, char **argv)
{
	int port = 9999;
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
			port = atoi(argv[++i]);
		} else if (n_backends < MAX_BACKENDS) {
			if (resolve_backend(argv[i], &backends[n_backends]) == 0) {
				fprintf(stderr, "lb: backend %s\n", backends[n_backends].host);
				n_backends++;
			} else {
				fprintf(stderr, "lb: bad backend spec %s\n", argv[i]);
			}
		}
	}
	if (n_backends == 0) {
		fprintf(stderr, "usage: %s [--port N] host:port [host:port ...]\n", argv[0]);
		return 2;
	}

	for (int i = 0; i < MAX_PAIRS; i++) {
		pairs[i].client_fd        = -1;
		pairs[i].backend_fd       = -1;
		pairs[i].c_to_b_pipe[0]   = -1; pairs[i].c_to_b_pipe[1] = -1;
		pairs[i].b_to_c_pipe[0]   = -1; pairs[i].b_to_c_pipe[1] = -1;
	}

	signal(SIGTERM, on_signal);
	signal(SIGINT,  on_signal);
	signal(SIGPIPE, SIG_IGN);

	listen_fd = setup_listener(port);
	if (listen_fd < 0) { perror("listen"); return 1; }
	fprintf(stderr, "lb: listening on :%d, %d backend(s)\n", port, n_backends);

	epfd = epoll_create1(EPOLL_CLOEXEC);
	if (epfd < 0) { perror("epoll_create1"); return 1; }
	struct epoll_event ev = {0};
	ev.events  = EPOLLIN;
	ev.data.fd = listen_fd;
	epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd, &ev);

	struct epoll_event events[64];

	while (!stop_flag) {
		int n = epoll_wait(epfd, events, 64, -1);
		if (n < 0) { if (errno == EINTR) continue; perror("epoll_wait"); break; }

		for (int i = 0; i < n; i++) {
			int fd = events[i].data.fd;

			if (fd == listen_fd) {
				for (;;) {
					int cfd = accept4(listen_fd, NULL, NULL,
					                  SOCK_NONBLOCK | SOCK_CLOEXEC);
					if (cfd < 0) break;
					int slot = find_free_pair();
					if (slot < 0) { close(cfd); continue; }
					int one = 1;
					setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

					int bidx = rr_next; rr_next = (rr_next + 1) % n_backends;
					int bfd  = connect_backend_nb(bidx);
					if (bfd < 0) { close(cfd); continue; }

					Pair *p = &pairs[slot];
					p->client_fd  = cfd;
					p->backend_fd = bfd;
					p->closed     = 0;
					if (pipe2(p->c_to_b_pipe, O_NONBLOCK | O_CLOEXEC) < 0 ||
					    pipe2(p->b_to_c_pipe, O_NONBLOCK | O_CLOEXEC) < 0) {
						pair_close(p); continue;
					}
					struct epoll_event cev = {0};
					cev.events  = EPOLLIN | EPOLLOUT | EPOLLET | EPOLLRDHUP;
					cev.data.fd = cfd;
					if (epoll_ctl(epfd, EPOLL_CTL_ADD, cfd, &cev) < 0) { pair_close(p); continue; }
					cev.data.fd = bfd;
					if (epoll_ctl(epfd, EPOLL_CTL_ADD, bfd, &cev) < 0) { pair_close(p); continue; }
				}
				continue;
			}

			int is_client;
			int idx = pair_index_for_fd(fd, &is_client);
			if (idx < 0) {
				epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
				close(fd);
				continue;
			}
			Pair *p = &pairs[idx];
			if (p->closed) continue;

			if (events[i].events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
				/* Try to drain any inflight data before closing. */
				if (p->c_to_b_inflight > 0)
					drain_pipe_to(p->c_to_b_pipe[0], p->backend_fd, &p->c_to_b_inflight);
				if (p->b_to_c_inflight > 0)
					drain_pipe_to(p->b_to_c_pipe[0], p->client_fd,  &p->b_to_c_inflight);
				pair_close(p);
				continue;
			}

			int rc;
			/* Client → backend */
			rc = forward(p->client_fd, p->c_to_b_pipe[1], p->c_to_b_pipe[0],
			             p->backend_fd, &p->c_to_b_inflight);
			if (rc < 0) { pair_close(p); continue; }
			/* Backend → client */
			rc = forward(p->backend_fd, p->b_to_c_pipe[1], p->b_to_c_pipe[0],
			             p->client_fd, &p->b_to_c_inflight);
			if (rc < 0) { pair_close(p); continue; }
		}
	}

	if (listen_fd >= 0) close(listen_fd);
	if (epfd >= 0) close(epfd);
	return 0;
}

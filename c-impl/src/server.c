/* epoll edge-triggered HTTP server.
 *
 * Single thread, single process. State per connection in a fixed pool.
 * Hot path: accept → read → parse → vectorize → search → write → keep-alive.
 *
 * Reliability: any parse/processing error → safe fallback response (200,
 * approved=true, fraud_score=0.0000). Never returns 4xx/5xx (HTTP errors
 * are weight 5 in the score formula vs 1 for FP).
 */

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "http.h"
#include "json.h"
#include "index.h"
#include "response.h"
#include "search.h"
#include "vectorize.h"

typedef enum {
	CS_READING = 0,
	CS_WRITING = 1,
} ConnState;

typedef struct {
	int       fd;          /* -1 = slot free */
	ConnState state;
	int       read_len;
	char      read_buf[READ_BUF_SIZE + 64]; /* +64 slack for line scans */
	const char *write_ptr;
	int       write_remaining;
} Conn;

static Conn  conns[MAX_CONNECTIONS];
static int   epfd = -1;
static int   listen_fd = -1;
static int   unix_listen_fd = -1;   /* AF_UNIX listener for LB FD handoff (optional) */
static int   lb_conn[8];            /* connected LB handoff channels (FD sources) */
static int   n_lb_conn = 0;
static volatile sig_atomic_t stop_flag = 0;

/* O(1) fd -> conn-slot map (replaces the per-event linear scan). Container fds
 * stay small (< a few hundred); sized generously. -1 = not a client conn. */
#define FD_SLOT_SIZE 65536
static int   fd_slot[FD_SLOT_SIZE];

/* Brief socket busy-poll: spin a few µs in epoll/recv before sleeping, cutting
 * wakeup-latency at the p99 tail. ~µs of spin at this request rate (~1% CPU),
 * so the 0.45-CPU quota is not at risk. setsockopt failures are ignored. */
#define BUSY_POLL_US 30

static void on_signal(int sig) { (void)sig; stop_flag = 1; }

static int find_free_slot(void)
{
	for (int i = 0; i < MAX_CONNECTIONS; i++)
		if (conns[i].fd < 0) return i;
	return -1;
}

static void conn_close(Conn *c)
{
	if (c->fd >= 0) {
		epoll_ctl(epfd, EPOLL_CTL_DEL, c->fd, NULL);
		if (c->fd < FD_SLOT_SIZE) fd_slot[c->fd] = -1;
		close(c->fd);
		c->fd = -1;
	}
}

/* Latency socket tuning applied to each client socket. All optional —
 * failures (EPERM/ENOPROTOOPT in restricted containers) are harmless. */
static void tune_client_socket(int cfd)
{
	int one = 1;
	setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
#ifdef TCP_QUICKACK
	setsockopt(cfd, IPPROTO_TCP, TCP_QUICKACK, &one, sizeof(one));
#endif
#ifdef SO_BUSY_POLL
	int bp = BUSY_POLL_US;
	setsockopt(cfd, SOL_SOCKET, SO_BUSY_POLL, &bp, sizeof(bp));
#endif
#ifdef SO_PREFER_BUSY_POLL
	setsockopt(cfd, SOL_SOCKET, SO_PREFER_BUSY_POLL, &one, sizeof(one));
#endif
}

static void conn_reset_for_next(Conn *c)
{
	c->state           = CS_READING;
	c->read_len        = 0;
	c->write_ptr       = NULL;
	c->write_remaining = 0;
}

/* Insert an accepted (TCP) or received (FD-handoff) client fd into the conn
 * pool + epoll. Shared by the TCP accept loop and the LB FD receiver. */
static void accept_client_fd(int cfd)
{
	int slot = find_free_slot();
	if (slot < 0) { close(cfd); return; }
	int fl = fcntl(cfd, F_GETFL, 0);
	if (fl >= 0) fcntl(cfd, F_SETFL, fl | O_NONBLOCK);  /* ET epoll needs nonblock */
	tune_client_socket(cfd);
	conns[slot].fd = cfd;
	if (cfd < FD_SLOT_SIZE) fd_slot[cfd] = slot;       /* O(1) lookup */
	conn_reset_for_next(&conns[slot]);
	struct epoll_event cev = {0};
	cev.events  = EPOLLIN | EPOLLOUT | EPOLLET | EPOLLRDHUP;
	cev.data.fd = cfd;
	if (epoll_ctl(epfd, EPOLL_CTL_ADD, cfd, &cev) < 0) {
		close(cfd); conns[slot].fd = -1;
		if (cfd < FD_SLOT_SIZE) fd_slot[cfd] = -1;
	}
}

/* Queue a response (pointer + length) for write. The buffer is static,
 * so we just remember the pointer + remaining. Returns 0 always. */
static void queue_write(Conn *c, const char *resp, int resp_len)
{
	c->write_ptr       = resp;
	c->write_remaining = resp_len;
	c->state           = CS_WRITING;
}

/* Process a complete request now in c->read_buf. Picks a response and
 * stages it for write. Always succeeds (falls back on any error path). */
static void handle_request(Conn *c, HttpRequest *req)
{
	if (req->route == HTTP_GET_READY) {
		queue_write(c, HTTP_READY_RESP, HTTP_READY_RESP_LEN);
		return;
	}
	if (req->route != HTTP_POST_FRAUD) {
		/* Unknown route. We don't issue 404 — would count as raw failure.
		 * Send the safe fallback 200 to keep things in the "FP weight 1" bucket. */
		queue_write(c, HTTP_RESPONSES[0], HTTP_RESPONSES_LEN[0]);
		return;
	}

	Payload p;
	if (json_parse(req->body, req->body_len, &p) < 0) {
		queue_write(c, HTTP_RESPONSES[0], HTTP_RESPONSES_LEN[0]);
		return;
	}

	int16_t vec[RECORD_STRIDE];
	vectorize_payload(&p, vec);

	SearchState st;
	Neighbor    neigh[K];
	search_knn(vec, &st, neigh);

	int fraud = 0;
	for (int i = 0; i < K; i++)
		if (is_fraud[neigh[i].node_idx] == 1) fraud++;

	queue_write(c, HTTP_RESPONSES[fraud], HTTP_RESPONSES_LEN[fraud]);
}

/* Drain everything possible from the read side; on each complete request,
 * stage a response, switch to writing. */
static void on_readable(Conn *c)
{
	for (;;) {
		ssize_t n = recv(c->fd, c->read_buf + c->read_len,
		                 READ_BUF_SIZE - c->read_len, 0);
		if (n > 0) {
			c->read_len += (int)n;
			if (c->read_len >= READ_BUF_SIZE) {
				/* Oversized request: safe fallback and close. */
				queue_write(c, HTTP_RESPONSES[0], HTTP_RESPONSES_LEN[0]);
				return;
			}
			continue;
		}
		if (n == 0) { conn_close(c); return; }
		if (errno == EAGAIN || errno == EWOULDBLOCK) break;
		conn_close(c);
		return;
	}

	/* Process as many complete requests as possible (pipelining). */
	while (c->read_len > 0 && c->state == CS_READING) {
		HttpRequest req;
		int rc = http_parse_headers(c->read_buf, c->read_len, &req);
		if (rc == 0) break;        /* need more data */
		if (rc < 0) {
			/* malformed headers: safe fallback, then drop conn after write */
			queue_write(c, HTTP_RESPONSES[0], HTTP_RESPONSES_LEN[0]);
			return;
		}
		int total = req.headers_len + req.content_length;
		if (total > READ_BUF_SIZE) {
			queue_write(c, HTTP_RESPONSES[0], HTTP_RESPONSES_LEN[0]);
			return;
		}
		if (c->read_len < total) break; /* body still arriving */
		if (req.content_length > 0) http_set_body(&req, c->read_buf);

		handle_request(c, &req);

		/* Shift any pipelined bytes to the start. */
		int leftover = c->read_len - total;
		if (leftover > 0)
			memmove(c->read_buf, c->read_buf + total, (size_t)leftover);
		c->read_len = leftover;
		break; /* one request at a time; resume after write completes */
	}
}

static void on_writable(Conn *c)
{
	while (c->write_remaining > 0) {
		ssize_t n = send(c->fd, c->write_ptr, (size_t)c->write_remaining, MSG_NOSIGNAL);
		if (n > 0) {
			c->write_ptr       += n;
			c->write_remaining -= (int)n;
			continue;
		}
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			/* Will be re-armed by EPOLLET on next writable event. */
			return;
		}
		conn_close(c);
		return;
	}
	/* Response fully sent; keep-alive — go back to reading. */
	conn_reset_for_next(c);
	/* Re-arm: edge-triggered means we may have pipelined data already buffered. */
	on_readable(c);
}

static int setup_listener(int port)
{
	int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (fd < 0) return -1;
	int one = 1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
	struct sockaddr_in addr = {0};
	addr.sin_family      = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port        = htons((uint16_t)port);
	if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) { close(fd); return -1; }
	if (listen(fd, 1024) < 0) { close(fd); return -1; }
	return fd;
}

/* AF_UNIX stream listener for the FD-passing LB. The API owns the socket
 * (created on a shared volume); the LB connects and hands off client FDs. */
static int setup_unix_listener(const char *path)
{
	unlink(path);
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (fd < 0) return -1;
	struct sockaddr_un sa = {0};
	sa.sun_family = AF_UNIX;
	snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", path);
	if (bind(fd, (struct sockaddr*)&sa, sizeof(sa)) < 0) { close(fd); return -1; }
	if (listen(fd, 16) < 0) { close(fd); return -1; }
	return fd;
}

static int is_lb_conn(int fd)
{
	for (int i = 0; i < n_lb_conn; i++) if (lb_conn[i] == fd) return 1;
	return 0;
}

static void remove_lb_conn(int fd)
{
	for (int i = 0; i < n_lb_conn; i++)
		if (lb_conn[i] == fd) { lb_conn[i] = lb_conn[--n_lb_conn]; return; }
}

/* Receive all pending client FDs handed off by the LB over `uxfd` (SCM_RIGHTS).
 * Returns 0 (drained) or -1 (LB closed/error -> caller drops the channel). */
static int drain_lb_fds(int uxfd)
{
	for (;;) {
		char dummy;
		struct iovec io = { .iov_base = &dummy, .iov_len = 1 };
		union { char buf[CMSG_SPACE(sizeof(int))]; struct cmsghdr align; } u;
		struct msghdr msg = {0};
		msg.msg_iov = &io; msg.msg_iovlen = 1;
		msg.msg_control = u.buf; msg.msg_controllen = sizeof(u.buf);
		ssize_t n = recvmsg(uxfd, &msg, 0);
		if (n > 0) {
			struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
			if (cmsg && cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
				int cfd; memcpy(&cfd, CMSG_DATA(cmsg), sizeof(int));
				accept_client_fd(cfd);
			}
			continue;
		}
		if (n == 0) return -1;                               /* LB closed */
		if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;/* drained */
		return -1;
	}
}

int server_run(int port)
{
	signal(SIGTERM, on_signal);
	signal(SIGINT,  on_signal);
	signal(SIGPIPE, SIG_IGN);

	for (int i = 0; i < MAX_CONNECTIONS; i++) conns[i].fd = -1;
	for (int i = 0; i < FD_SLOT_SIZE; i++) fd_slot[i] = -1;

	listen_fd = setup_listener(port);
	if (listen_fd < 0) { perror("listen"); return -1; }
	fprintf(stderr, "server: listening on :%d (epoll, edge-triggered)\n", port);

	epfd = epoll_create1(EPOLL_CLOEXEC);
	if (epfd < 0) { perror("epoll_create1"); return -1; }

	struct epoll_event ev = {0};
	ev.events  = EPOLLIN;
	ev.data.fd = listen_fd;  /* listener uses fd as marker */
	if (epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd, &ev) < 0) { perror("epoll_ctl listen"); return -1; }

	/* Optional FD-handoff channel: if FDSOCK is set, also accept client FDs
	 * passed by the LB over a unix socket (single-hop, LB out of the data path).
	 * The TCP listener above stays up for the /ready healthcheck. */
	const char *fdsock = getenv("FDSOCK");
	if (fdsock && fdsock[0]) {
		unix_listen_fd = setup_unix_listener(fdsock);
		if (unix_listen_fd < 0) { perror("unix listen"); return -1; }
		struct epoll_event uev = {0};
		uev.events = EPOLLIN; uev.data.fd = unix_listen_fd;
		epoll_ctl(epfd, EPOLL_CTL_ADD, unix_listen_fd, &uev);
		fprintf(stderr, "server: FD-handoff listening on %s\n", fdsock);
	}

	struct epoll_event events[64];

	while (!stop_flag) {
		int n = epoll_wait(epfd, events, 64, -1);
		if (n < 0) {
			if (errno == EINTR) continue;
			perror("epoll_wait");
			break;
		}
		for (int i = 0; i < n; i++) {
			int fd = events[i].data.fd;

			if (fd == listen_fd) {
				/* TCP accept loop (edge-triggered, drain). */
				for (;;) {
					int cfd = accept4(listen_fd, NULL, NULL,
					                  SOCK_NONBLOCK | SOCK_CLOEXEC);
					if (cfd < 0) break;
					accept_client_fd(cfd);
				}
				continue;
			}

			if (fd == unix_listen_fd) {
				/* LB connected its FD-handoff channel — accept it. */
				for (;;) {
					int lfd = accept4(unix_listen_fd, NULL, NULL,
					                  SOCK_NONBLOCK | SOCK_CLOEXEC);
					if (lfd < 0) break;
					if (n_lb_conn < (int)(sizeof(lb_conn)/sizeof(lb_conn[0]))) {
						lb_conn[n_lb_conn++] = lfd;
						struct epoll_event lev = {0};
						lev.events = EPOLLIN | EPOLLET;
						lev.data.fd = lfd;
						if (epoll_ctl(epfd, EPOLL_CTL_ADD, lfd, &lev) < 0) {
							close(lfd); n_lb_conn--;
						}
					} else { close(lfd); }
				}
				continue;
			}

			if (is_lb_conn(fd)) {
				/* Drain handed-off client FDs into the conn pool. */
				if (drain_lb_fds(fd) < 0) {
					epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
					close(fd);
					remove_lb_conn(fd);
				}
				continue;
			}

			/* O(1) fd -> conn slot (verified against the slot's fd). */
			Conn *c = NULL;
			if (fd >= 0 && fd < FD_SLOT_SIZE) {
				int s = fd_slot[fd];
				if (s >= 0 && s < MAX_CONNECTIONS && conns[s].fd == fd) c = &conns[s];
			}
			if (!c) {
				/* Stale event: defensively unregister. */
				epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
				close(fd);
				continue;
			}

			if (events[i].events & (EPOLLHUP | EPOLLRDHUP | EPOLLERR)) {
				conn_close(c);
				continue;
			}
			if (c->state == CS_READING && (events[i].events & EPOLLIN)) {
				on_readable(c);
			}
			if (c->state == CS_WRITING && (events[i].events & EPOLLOUT)) {
				on_writable(c);
			}
		}
	}

	if (listen_fd >= 0) close(listen_fd);
	if (epfd >= 0) close(epfd);
	return 0;
}

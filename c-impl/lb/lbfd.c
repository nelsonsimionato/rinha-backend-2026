/* FD-passing load balancer (single-hop).
 *
 *   ./lbfd --port 9999 /sock/api1.sock /sock/api2.sock
 *
 * - Binds :9999, accept4() client TCP connections (bridge network).
 * - Round-robins each NEW connection to an API by passing the client socket
 *   FD via SCM_RIGHTS over a connected AF_UNIX stream (one per API, on a shared
 *   volume). The API then serves that client DIRECTLY for the life of the
 *   keep-alive connection — the LB is out of the data path after handoff.
 * - The LB never reads/writes client payload: zero proxy hops on the data path.
 *
 * The API unix sockets live on a shared tmpfs volume; the API creates them,
 * the LB connects (with retry). All containers stay on the bridge network.
 */
#define _GNU_SOURCE
#include <errno.h>
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

#define MAX_BACKENDS 16

static const char *sock_paths[MAX_BACKENDS];
static int         api_fd[MAX_BACKENDS];   /* connected AF_UNIX stream to each API */
static int         n_backends = 0;
static int         rr_next = 0;
static volatile sig_atomic_t stop_flag = 0;

static void on_signal(int s) { (void)s; stop_flag = 1; }

/* Connect an AF_UNIX stream to `path`, retrying a few seconds (the API may not
 * have created the socket yet at startup). Returns fd or -1. */
static int connect_unix(const char *path)
{
	struct sockaddr_un sa = {0};
	sa.sun_family = AF_UNIX;
	snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", path);
	for (int attempt = 0; attempt < 600; attempt++) {  /* up to ~60s */
		int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
		if (fd < 0) return -1;
		if (connect(fd, (struct sockaddr*)&sa, sizeof(sa)) == 0) return fd;
		close(fd);
		usleep(100000);  /* 100ms */
	}
	return -1;
}

/* Send one fd to a connected unix socket via SCM_RIGHTS (+1 dummy byte). */
static int send_fd(int uxfd, int fd)
{
	char dummy = 'F';
	struct iovec io = { .iov_base = &dummy, .iov_len = 1 };
	union { char buf[CMSG_SPACE(sizeof(int))]; struct cmsghdr align; } u = {0};
	struct msghdr msg = {0};
	msg.msg_iov = &io; msg.msg_iovlen = 1;
	msg.msg_control = u.buf; msg.msg_controllen = sizeof(u.buf);
	struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
	cmsg->cmsg_level = SOL_SOCKET;
	cmsg->cmsg_type  = SCM_RIGHTS;
	cmsg->cmsg_len   = CMSG_LEN(sizeof(int));
	memcpy(CMSG_DATA(cmsg), &fd, sizeof(int));
	for (;;) {
		ssize_t n = sendmsg(uxfd, &msg, MSG_NOSIGNAL);
		if (n >= 0) return 0;
		if (errno == EINTR) continue;
		return -1;
	}
}

static int setup_listener(int port)
{
	int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (fd < 0) return -1;
	int one = 1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	struct sockaddr_in addr = {0};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port = htons((uint16_t)port);
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
			sock_paths[n_backends++] = argv[i];
		}
	}
	if (n_backends == 0) {
		fprintf(stderr, "usage: %s [--port N] /path/api1.sock [/path/api2.sock ...]\n", argv[0]);
		return 2;
	}

	signal(SIGTERM, on_signal);
	signal(SIGINT,  on_signal);
	signal(SIGPIPE, SIG_IGN);

	for (int i = 0; i < n_backends; i++) {
		api_fd[i] = connect_unix(sock_paths[i]);
		if (api_fd[i] < 0) {
			fprintf(stderr, "lbfd: cannot connect %s\n", sock_paths[i]);
			return 1;
		}
		fprintf(stderr, "lbfd: connected backend %s\n", sock_paths[i]);
	}

	int listen_fd = setup_listener(port);
	if (listen_fd < 0) { perror("listen"); return 1; }
	fprintf(stderr, "lbfd: listening on :%d, %d backend(s) (FD-passing)\n", port, n_backends);

	int epfd = epoll_create1(EPOLL_CLOEXEC);
	struct epoll_event ev = {0};
	ev.events = EPOLLIN; ev.data.fd = listen_fd;
	epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd, &ev);

	struct epoll_event events[64];
	while (!stop_flag) {
		int n = epoll_wait(epfd, events, 64, -1);
		if (n < 0) { if (errno == EINTR) continue; break; }
		for (int i = 0; i < n; i++) {
			if (events[i].data.fd != listen_fd) continue;
			for (;;) {
				int cfd = accept4(listen_fd, NULL, NULL, SOCK_CLOEXEC);
				if (cfd < 0) break;
				int one = 1;
				setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
				int b = rr_next; rr_next = (rr_next + 1) % n_backends;
				if (send_fd(api_fd[b], cfd) < 0) {
					/* try the other backend(s) once before giving up */
					int sent = 0;
					for (int k = 1; k < n_backends; k++) {
						int j = (b + k) % n_backends;
						if (send_fd(api_fd[j], cfd) == 0) { sent = 1; break; }
					}
					if (!sent) fprintf(stderr, "lbfd: handoff failed\n");
				}
				close(cfd);  /* API owns it now */
			}
		}
	}
	close(listen_fd);
	return 0;
}

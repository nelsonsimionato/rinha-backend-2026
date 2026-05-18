#ifndef SERVER_H
#define SERVER_H

#include "compat.h"

/* Blocking call: opens a listening socket on `port`, runs the epoll loop
 * forever. Returns only on fatal error or SIGTERM. */
int server_run(int port);

#endif /* SERVER_H */

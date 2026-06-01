#ifndef SERVER_H
#define SERVER_H

#include "compat.h"

/* Blocking call: opens a listening socket on `port`, runs the epoll loop
 * forever. Returns only on fatal error or SIGTERM.
 * The epoll_wait timeout is read once from env EPOLL_TIMEOUT_MS (default -1 =
 * block). Set to 1 to poll every 1ms — keeps a pinned/dedicated core out of
 * deep idle and the vCPU runnable, cutting steal/wakeup latency on the p99. */
int server_run(int port);

/* Optional startup warmup: run `n_queries` searches over sample index vectors
 * to warm the search code path (branch predictors, I-cache, hot data) before
 * real traffic. No-op if n_queries <= 0. Index must be loaded first. */
void server_warmup(int n_queries);

#endif /* SERVER_H */

#ifndef _LIGHTTPD_BACKENDS_H_
#define _LIGHTTPD_BACKENDS_H_

#include <lighttpd/base.h>

enum liBackendResult {
	LI_BACKEND_SUCCESS, /* got a connection */
	LI_BACKEND_WAIT, /* establishing new connection, or waiting for a free slot */
	LI_BACKEND_TIMEOUT /* wait timed out, no free slots available */
};

typedef enum liBackendResult liBackendResult;
typedef struct liBackendCallbacks liBackendCallbacks;
typedef struct liBackendWait liBackendWait;
typedef struct liBackendConnection liBackendConnection;
typedef struct liBackendPool liBackendPool;
typedef struct liBackendConfig liBackendConfig;

typedef void (*liBackendConnectionThreadCB)(liBackendPool *bpool, liWorker *wrk, liBackendConnection *bcon);
typedef void (*liBackendCB)(liBackendPool *bpool);


struct liBackendConnection {
	liEventIO watcher;
	gpointer data;
};

/* states: [start]  ->(new)->   [INACTIVE]  ->(detach)->   [detached]  ->(attach)->   [INACTIVE]   ->get->   [active]   ->put->   [INACTIVE]   ->(close)->   [done] */
/* the backend pool might be locked while the callbacks are running, but don't rely on it */
struct liBackendCallbacks {
	/* for moving connection between threads */
	liBackendConnectionThreadCB detach_thread_cb;
	liBackendConnectionThreadCB attach_thread_cb;

	/* for initializing/shutdown */
	liBackendConnectionThreadCB new_cb;
	liBackendConnectionThreadCB close_cb;

	/* free pool config */
	liBackendCB free_cb;
};


struct liBackendPool {
	/* READ ONLY CONFIGURATION DATA */
	const liBackendConfig *config;
};

struct liBackendConfig {
	const liBackendCallbacks *callbacks;

	liSocketAddress sock_addr;

	/* >0: real limit for current connections + pending connects
	 * <0: unlimited connections, absolute value limits the number of pending connects per worker
	 * =0: no limit
	 *
	 * if there is no limit (i.e. <= 0), backend connections won't be moved between threads
	 */
	int max_connections;

	/* how long we wait on keep-alive connections. 0: no keep-alive;
	 * also used for new connection we didn't use
	 */
	guint idle_timeout;

	/* how long we wait for connect to succeed, must be > 0; when connect fails the pool gets "disabled". */
	guint connect_timeout;

	/* how long a vrequest is allowed to wait for a connect  before we return an error. if pool gets disabled all requests fail.
	 * if a pending connect is assigned to a vrequest wait_timeout is not active.
	 */
	guint wait_timeout;

	/* how long the pool stays disabled. even if this is 0, all vrequests will receive an error on disable */
	guint disable_time;

	/* max requests per connection. -1: unlimited */
	int max_requests;

	/* if enabled, the backend.watcher will be set to internal callback and LI_EV_READ while the connection
	 * is not used by a vrequest;
	 *   if it sees input data it will log an error and close it, and if it sees eof it will
	 *   close it too
	 * if you disable this you should have to handle this yourself
	 */
	gboolean watch_for_close;
};

LI_API liBackendPool* li_backend_pool_new(const liBackendConfig *config);
LI_API void li_backend_pool_free(liBackendPool *bpool);

LI_API liBackendResult li_backend_get(liVRequest *vr, liBackendPool *bpool, liBackendConnection **pbcon, liBackendWait **pbwait);
LI_API void li_backend_wait_stop(liVRequest *vr, liBackendPool *bpool, liBackendWait **pbwait);

/* set bcon->fd = -1 if you closed the connection after an error */
LI_API void li_backend_put(liWorker *wrk, liBackendPool *bpool, liBackendConnection *bcon, gboolean closecon); /* if closecon == TRUE or bcon->watcher.fd == -1 the connection gets removed */

/* if an idle connections gets closed; bcon must be INACTIVE (i.e. not detached and not active).
 * call in worker that bcon is attached to.
 */
LI_API void li_backend_connection_closed(liBackendPool *bpool, liBackendConnection *bcon);

#endif

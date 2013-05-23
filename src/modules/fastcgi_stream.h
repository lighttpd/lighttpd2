#ifndef _LIGHTTPD_FASTCGI_STREAM_H_
#define _LIGHTTPD_FASTCGI_STREAM_H_

#include <lighttpd/base.h>
#include <lighttpd/backends.h>

typedef struct liFastCGIBackendCallbacks liFastCGIBackendCallbacks;
typedef struct liFastCGIBackendWait liFastCGIBackendWait;
typedef struct liFastCGIBackendConnection liFastCGIBackendConnection;
typedef struct liFastCGIBackendPool liFastCGIBackendPool;
typedef struct liFastCGIBackendConfig liFastCGIBackendConfig;

typedef void (*liFastCGIBackendConnectionResetCB)(liVRequest *vr, liFastCGIBackendPool *pool, liFastCGIBackendConnection *bcon);
typedef void (*liFastCGIBackendConnectionEndRequestCB)(liVRequest *vr, liFastCGIBackendPool *pool, liFastCGIBackendConnection *bcon, guint32 appStatus);
typedef void (*liFastCGIBackendConnectionStderrCB)(liVRequest *vr, liFastCGIBackendPool *pool, liFastCGIBackendConnection *bcon, GString *message);


struct liFastCGIBackendConnection {
	gpointer data;
};

struct liFastCGIBackendCallbacks {
	liFastCGIBackendConnectionResetCB reset_cb;
	liFastCGIBackendConnectionEndRequestCB end_request_cb;
	liFastCGIBackendConnectionStderrCB fastcgi_stderr_cb;
};

struct liFastCGIBackendPool {
	liBackendPool *subpool;
};

struct liFastCGIBackendConfig {
	const liFastCGIBackendCallbacks *callbacks;

	/* see liBackendConfig */
	liSocketAddress sock_addr;
	int max_connections;
	guint idle_timeout;
	guint connect_timeout;
	guint wait_timeout;
	guint disable_time;
	int max_requests;
};

/* config gets copied, can be freed after this call */
LI_API liFastCGIBackendPool* li_fastcgi_backend_pool_new(const liFastCGIBackendConfig *config);
LI_API void li_fastcgi_backend_pool_free(liFastCGIBackendPool *bpool);

LI_API liBackendResult li_fastcgi_backend_get(liVRequest *vr, liFastCGIBackendPool *bpool, liFastCGIBackendConnection **pbcon, liFastCGIBackendWait **pbwait);
LI_API void li_fastcgi_backend_wait_stop(liVRequest *vr, liFastCGIBackendPool *bpool, liFastCGIBackendWait **pbwait);

/* only call from reset or end_request callbacks */
LI_API void li_fastcgi_backend_put(liFastCGIBackendConnection *bcon);

#endif

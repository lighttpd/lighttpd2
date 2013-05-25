#ifndef _LIGHTTPD_THROTTLE_H_
#define _LIGHTTPD_THROTTLE_H_

#include <lighttpd/base.h>

#define LI_THROTTLE_GRANULARITY 200 /* defines how frequently (in milliseconds) a magazine is refilled */

typedef void (*liThrottleNotifyCB)(liThrottleState *state, gpointer data);

LI_API liThrottleState* li_throttle_new(void);
LI_API void li_throttle_set(liWorker *wrk, liThrottleState *state, guint rate, guint burst);
LI_API void li_throttle_free(liWorker *wrk, liThrottleState *state);

LI_API guint li_throttle_query(liWorker *wrk, liThrottleState *state, guint interested, liThrottleNotifyCB notify_callback, gpointer data);
LI_API void li_throttle_update(liThrottleState *state, guint used);

LI_API liThrottlePool* li_throttle_pool_new(liServer *srv, guint rate, guint burst);
LI_API void li_throttle_pool_acquire(liThrottlePool *pool);
LI_API void li_throttle_pool_release(liThrottlePool *pool, liServer *srv);

/* returns whether pool was actually added (otherwise it already was added) */
LI_API gboolean li_throttle_add_pool(liWorker *wrk, liThrottleState *state, liThrottlePool *pool);
LI_API void li_throttle_remove_pool(liWorker *wrk, liThrottleState *state, liThrottlePool *pool);

/* internal for worker waitqueue setup */
LI_API void li_throttle_waitqueue_cb(liWaitQueue *wq, gpointer data);

#endif

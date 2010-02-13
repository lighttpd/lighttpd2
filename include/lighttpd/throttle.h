#ifndef _LIGHTTPD_THROTTLE_H_
#define _LIGHTTPD_THROTTLE_H_

#define THROTTLE_GRANULARITY 0.2 /* defines how frequently a magazine is refilled. should be 0.1 <= x <= 1.0 */

typedef struct liThrottlePool liThrottlePool;
struct liThrottlePool {
	GString *name;
	guint rate; /** bytes/s */
	gint magazine;
	GQueue** queues;  /** worker specific queues */
	gint num_cons_queued;

	gint rearming;
	ev_tstamp last_pool_rearm;
	ev_tstamp *last_con_rearm;
};

typedef struct liThrottleParam liThrottleParam;
struct liThrottleParam {
	guint rate;
	guint burst;
};

LI_API void li_throttle_reset(liConnection *con);
LI_API void li_throttle_cb(struct ev_loop *loop, ev_timer *w, int revents);

LI_API liThrottlePool *li_throttle_pool_new(liServer *srv, GString *name, guint rate);
LI_API void li_throttle_pool_free(liServer *srv, liThrottlePool *pool);

LI_API void li_throttle_pool_acquire(liConnection *con, liThrottlePool *pool);
LI_API void li_throttle_pool_release(liConnection *con);

#endif

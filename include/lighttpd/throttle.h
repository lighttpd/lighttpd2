#ifndef _LIGHTTPD_THROTTLE_H_
#define _LIGHTTPD_THROTTLE_H_

#define THROTTLE_GRANULARITY 0.2 /* defines how frequently a magazine is refilled. should be 0.1 <= x <= 1.0 */

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

struct liThrottleParam {
	guint rate;
	guint burst;
};

LI_API void li_throttle_reset(liVRequest *vr);
LI_API void li_throttle_cb(struct ev_loop *loop, ev_timer *w, int revents);

LI_API liThrottlePool *li_throttle_pool_new(liServer *srv, GString *name, guint rate);
LI_API void li_throttle_pool_free(liServer *srv, liThrottlePool *pool);

LI_API void li_throttle_pool_acquire(liVRequest *vr, liThrottlePool *pool);
LI_API void li_throttle_pool_release(liVRequest *vr);

/* update throttle data: notify it that we sent <transferred> bytes, and that we never send more than write_max at once */
LI_API void li_throttle_update(liVRequest *vr, goffset transferred, goffset write_max);

#endif

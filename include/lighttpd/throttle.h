#ifndef _LIGHTTPD_THROTTLE_H_
#define _LIGHTTPD_THROTTLE_H_

#define THROTTLE_GRANULARITY 200 /* defines how frequently (in milliseconds) a magazine is refilled */

/* this makro converts a ev_tstamp to a gint. this is needed for atomic access. millisecond precision, can hold a month max */
#define THROTTLE_EVTSTAMP_TO_GINT(x) ((gint) ((x - ((gint)x - (gint)x % (3600*24*31))) * 1000))

struct liThrottlePool {
	/* global per pool */
	GString *name;
	gint rate; /* bytes/s */
	gint refcount;
	gint rearming; /* atomic access, 1 if a worker is currently rearming the magazine */
	gint last_rearm; /* gint for atomic access. represents a ((gint)ev_tstamp*1000) */

	/* local per worker */
	gint *worker_magazine;
	gint *worker_last_rearm;
	gint *worker_num_cons_queued;
	GQueue** worker_queues;
};

struct liThrottleParam {
	gint rate;
	guint burst;
};

LI_API void li_throttle_reset(liVRequest *vr);
LI_API void li_throttle_cb(liWaitQueue *wq, gpointer data);

LI_API liThrottlePool *li_throttle_pool_new(liServer *srv, GString *name, guint rate);
LI_API void li_throttle_pool_free(liServer *srv, liThrottlePool *pool);

LI_API void li_throttle_pool_acquire(liVRequest *vr, liThrottlePool *pool);
LI_API void li_throttle_pool_release(liVRequest *vr);

/* update throttle data: notify it that we sent <transferred> bytes, and that we never send more than write_max at once */
LI_API void li_throttle_update(liVRequest *vr, goffset transferred, goffset write_max);

#endif

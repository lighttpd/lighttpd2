#ifndef _LIGHTTPD_THROTTLE_H_
#define _LIGHTTPD_THROTTLE_H_

#define THROTTLE_GRANULARITY 200 /* defines how frequently (in milliseconds) a magazine is refilled */

/* this makro converts a li_tstamp to a gint. this is needed for atomic access. millisecond precision, can hold two weeks max */
#define THROTTLE_EVTSTAMP_TO_GINT(x) ((gint) ((x - ((gint)x - (gint)x % (3600*24*14))) * 1000))

typedef struct liThrottleState liThrottleState;

/* vrequest data */
#if 0
	/* I/O throttling */
	gboolean throttled; /* TRUE if vrequest is throttled */
	struct {
		gint magazine; /* currently available for use */

		struct {
			liThrottlePool *ptr; /* NULL if not in any throttling pool */
			GList lnk;
			GQueue *queue;
			gint magazine;
		} pool;
		struct {
			liThrottlePool *ptr;
			GList lnk;
			GQueue *queue;
			gint magazine;
		} ip;
		struct {
			gint rate; /* maximum transfer rate in bytes per second, 0 if unlimited */
			ev_tstamp last_update;
		} con;
		liWaitQueueElem wqueue_elem;
	} throttle;
#endif

typedef enum {
	LI_THROTTLE_POOL_NAME,
	LI_THROTTLE_POOL_IP
} liThrottlePoolType;

struct liThrottlePool {
	/* global per pool */
	liThrottlePoolType type;
	union {
		GString *name;
		liSocketAddress addr;
	} data;
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

LI_API liThrottlePool *li_throttle_pool_new(liServer *srv, liThrottlePoolType type, gpointer param, guint rate);
LI_API void li_throttle_pool_free(liServer *srv, liThrottlePool *pool);

LI_API void li_throttle_pool_acquire(liVRequest *vr, liThrottlePool *pool);
LI_API void li_throttle_pool_release(liVRequest *vr, liThrottlePool *pool);

/* update throttle data: notify it that we sent <transferred> bytes, and that we never send more than write_max at once */
LI_API void li_throttle_update(liVRequest *vr, goffset transferred, goffset write_max);

#endif

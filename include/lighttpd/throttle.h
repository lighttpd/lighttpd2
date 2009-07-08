#ifndef _LIGHTTPD_THROTTLE_H_
#define _LIGHTTPD_THROTTLE_H_

#define THROTTLE_GRANULARITY 0.2 /* defines how frequently a magazine is refilled. should be 0.1 <= x <= 1.0 */

typedef struct liThrottlePool liThrottlePool;
struct liThrottlePool {
	GString *name;
	guint rate; /** bytes/s */
	gint magazine;
	GQueue** queues;  /** worker specific queues. each worker has 2 */
	guint* current_queue;
	gint num_cons;

	gint rearming;
	ev_tstamp last_pool_rearm;
	ev_tstamp *last_con_rearm;
};

void throttle_cb(struct ev_loop *loop, ev_timer *w, int revents);

liThrottlePool *throttle_pool_new(liServer *srv, GString *name, guint rate);
void throttle_pool_free(liServer *srv, liThrottlePool *pool);

#endif

#ifndef _LIGHTTPD_THROTTLE_H_
#define _LIGHTTPD_THROTTLE_H_

#define THROTTLE_GRANULARITY 0.2 /* defines how frequently a magazine is refilled. should be 0.1 <= x <= 1.0 */

struct throttle_pool_t {
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

typedef struct throttle_pool_t throttle_pool_t;

void throttle_cb(struct ev_loop *loop, ev_timer *w, int revents);

throttle_pool_t *throttle_pool_new(server *srv, GString *name, guint rate);
void throttle_pool_free(server *srv, throttle_pool_t *pool);

#endif

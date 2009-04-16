/*
 * Implemented with token bucket algorithm.
 * On average, the rate of bytes/s never exceeds the specified limit
 * but allows small bursts of previously unused bandwidth (max rate*2 for 1 second).
 */

#include <lighttpd/base.h>


throttle_pool_t *throttle_pool_new(server *srv, GString *name, guint rate) {
	throttle_pool_t *pool;
	guint i;

	pool = g_slice_new0(throttle_pool_t);
	pool->rate = rate;
	pool->magazine = rate * THROTTLE_GRANULARITY;
	pool->name = name;

	pool->queues = g_new0(GQueue*, srv->worker_count * 2);;
	for (i = 0; i < (srv->worker_count*2); i+=2) {
		pool->queues[i] = g_queue_new();
		pool->queues[i+1] = g_queue_new();
	}

	pool->current_queue = g_new0(guint, srv->worker_count);

	pool->last_pool_rearm = ev_time();
	pool->last_con_rearm = g_new0(ev_tstamp, srv->worker_count);
	for (i = 0; i < srv->worker_count; i++) {
		pool->last_con_rearm[i] = pool->last_pool_rearm;
	}

	return pool;
}

void throttle_pool_free(server *srv, throttle_pool_t *pool) {
	guint i;

	for (i = 0; i < (srv->workers->len*2); i+=2) {
		g_queue_free(pool->queues[i]);
		g_queue_free(pool->queues[i+1]);
	}
	g_free(pool->queues);

	g_free(pool->current_queue);
	g_free(pool->last_con_rearm);

	g_string_free(pool->name, TRUE);

	g_slice_free(throttle_pool_t, pool);
}


void throttle_cb(struct ev_loop *loop, ev_timer *w, int revents) {
	waitqueue_elem *wqe;
	throttle_pool_t *pool;
	connection *con;
	worker *wrk;
	ev_tstamp now;
	guint magazine, supply;
	GQueue *queue;
	GList *lnk, *lnk_next;

	UNUSED(revents);

	wrk = w->data;
	now = ev_now(loop);

	while (NULL != (wqe = waitqueue_pop(&wrk->throttle_queue))) {
		con = wqe->data;

		if (con->throttle.pool.ptr) {
			/* throttled by pool */
			pool = con->throttle.pool.ptr;

			/* this is basically another way to do "if (try_lock(foo)) { ...; unlock(foo); }" */
			if (g_atomic_int_compare_and_exchange(&pool->rearming, 0, 1)) {
				if ((now - pool->last_pool_rearm) >= THROTTLE_GRANULARITY) {
					if (g_atomic_int_get(&pool->magazine) <= (pool->rate * THROTTLE_GRANULARITY * 4))
						g_atomic_int_add(&pool->magazine, pool->rate * THROTTLE_GRANULARITY);

					pool->last_pool_rearm = now;
				}

				g_atomic_int_set(&pool->rearming, 0);
			}

			/* select current queue */
			queue = pool->queues[wrk->ndx*2+pool->current_queue[wrk->ndx]];

			if ((now - pool->last_con_rearm[wrk->ndx]) >= THROTTLE_GRANULARITY) {
				/* switch current queue by xoring with 1 */
				pool->current_queue[wrk->ndx] ^= 1;

				if (queue->length) {
					do {
						magazine = g_atomic_int_get(&pool->magazine);
						supply = magazine / g_atomic_int_get(&pool->num_cons);
					} while (!g_atomic_int_compare_and_exchange(&pool->magazine, magazine, magazine - (supply * queue->length)));

					g_atomic_int_add(&(pool->num_cons), - queue->length);

					/* rearm connections */
					for (lnk = g_queue_peek_head_link(queue); lnk != NULL; lnk = lnk_next) {
						((connection*)lnk->data)->throttle.pool.magazine += supply;
						((connection*)lnk->data)->throttle.pool.queued = FALSE;
						lnk_next = lnk->next;
						lnk->next = NULL;
						lnk->prev = NULL;
					}

					/* clear current connection queue */
					g_queue_init(queue);
				}

				pool->last_con_rearm[wrk->ndx] = now;
			}

			if (con->throttle.con.rate) {
				supply = MIN(con->throttle.pool.magazine, con->throttle.con.rate * THROTTLE_GRANULARITY);
				con->throttle.con.magazine += supply;
				con->throttle.pool.magazine -= supply;
			} else {
				con->throttle.con.magazine += con->throttle.pool.magazine;
				con->throttle.pool.magazine = 0;
			}
		} else if (con->throttle.ip.ptr) {
			/* throttled by ip */
		} else {
			/* throttled by connection */
			if (con->throttle.con.magazine <= con->throttle.con.rate * THROTTLE_GRANULARITY * 4)
				con->throttle.con.magazine += con->throttle.con.rate * THROTTLE_GRANULARITY;
		}

		ev_io_add_events(loop, &con->sock_watcher, EV_WRITE);
	}

	waitqueue_update(&wrk->throttle_queue);
}
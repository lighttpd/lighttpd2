/*
 * Implemented with token bucket algorithm.
 * On average, the rate of bytes/s never exceeds the specified limit
 * but allows small bursts of previously unused bandwidth (max rate*2 for 1 second).
 */

#include <lighttpd/base.h>


liThrottlePool *li_throttle_pool_new(liServer *srv, GString *name, guint rate) {
	liThrottlePool *pool;
	guint i;
	guint worker_count;

	worker_count = srv->worker_count ? srv->worker_count : 1;

	pool = g_slice_new0(liThrottlePool);
	pool->rate = rate;
	pool->magazine = rate * THROTTLE_GRANULARITY;
	pool->name = name;

	pool->queues = g_new0(GQueue*, worker_count);;
	for (i = 0; i < worker_count; i++) {
		pool->queues[i] = g_queue_new();
	}

	pool->last_pool_rearm = ev_time();
	pool->last_con_rearm = g_new0(ev_tstamp, worker_count);
	for (i = 0; i < worker_count; i++) {
		pool->last_con_rearm[i] = pool->last_pool_rearm;
	}

	return pool;
}

void li_throttle_pool_free(liServer *srv, liThrottlePool *pool) {
	guint i;
	guint worker_count;

	worker_count = srv->worker_count ? srv->worker_count : 1;

	for (i = 0; i < worker_count; i++) {
		g_queue_free(pool->queues[i]);
	}

	g_free(pool->queues);
	g_free(pool->last_con_rearm);
	g_string_free(pool->name, TRUE);
	g_slice_free(liThrottlePool, pool);
}

void li_throttle_pool_acquire(liConnection *con, liThrottlePool *pool) {
	gint magazine;

	if (con->throttle.pool.ptr == pool)
		return;

	if (con->throttle.pool.ptr != NULL) {
		/* already in a different pool */
		li_throttle_pool_release(con);
	}

	/* try to steal some initial 4kbytes from the pool */
	while ((magazine = g_atomic_int_get(&pool->magazine)) > (4*1024)) {
		if (g_atomic_int_compare_and_exchange(&pool->magazine, magazine, magazine - (4*1024))) {
			con->throttle.pool.magazine = 4*1024;
			break;
		}
	}

	con->throttle.pool.ptr = pool;
	con->throttled = TRUE;
}

void li_throttle_pool_release(liConnection *con) {
	if (con->throttle.pool.queue == NULL)
		return;

	if (con->throttle.pool.queue) {
		g_queue_unlink(con->throttle.pool.queue, &con->throttle.pool.lnk);
		con->throttle.pool.queue = NULL;
		g_atomic_int_add(&con->throttle.pool.ptr->num_cons_queued, -1);
	}

	/* give back bandwidth */
	g_atomic_int_add(&con->throttle.pool.ptr->magazine, con->throttle.pool.magazine);
	con->throttle.pool.magazine = 0;
	con->throttle.pool.ptr = NULL;
}

void li_throttle_pool_rearm(liWorker *wrk, liThrottlePool *pool) {
	ev_tstamp now = CUR_TS(wrk);

	/* this is basically another way to do "if (try_lock(foo)) { ...; unlock(foo); }" */
	if (g_atomic_int_compare_and_exchange(&pool->rearming, 0, 1)) {
		if ((now - pool->last_pool_rearm) >= THROTTLE_GRANULARITY) {
			if (g_atomic_int_get(&pool->magazine) <= (pool->rate * THROTTLE_GRANULARITY * 4)) {
				g_atomic_int_add(&pool->magazine, pool->rate * THROTTLE_GRANULARITY);

				pool->last_pool_rearm = now;
			}
		}

		g_atomic_int_set(&pool->rearming, 0);
	}

	if ((now - pool->last_con_rearm[wrk->ndx]) >= THROTTLE_GRANULARITY) {
		GQueue *queue;
		GList *lnk, *lnk_next;
		gint magazine, supply, num_cons;

		/* select current queue */
		queue = pool->queues[wrk->ndx];

		if (queue->length) {
			do {
				magazine = g_atomic_int_get(&pool->magazine);
				num_cons = g_atomic_int_get(&pool->num_cons_queued);
				supply = magazine / num_cons;
			} while (!g_atomic_int_compare_and_exchange(&pool->magazine, magazine, magazine - (supply * queue->length)));

			g_atomic_int_add(&(pool->num_cons_queued), - queue->length);

			/* rearm connections */
			for (lnk = g_queue_peek_head_link(queue); lnk != NULL; lnk = lnk_next) {
				((liConnection*)lnk->data)->throttle.pool.magazine += supply;
				((liConnection*)lnk->data)->throttle.pool.queue = NULL;
				lnk_next = lnk->next;
				lnk->next = NULL;
				lnk->prev = NULL;
			}

			/* clear current connection queue */
			g_queue_init(queue);
		}

		pool->last_con_rearm[wrk->ndx] = now;
	}
}

void li_throttle_reset(liConnection *con) {
	if (!con->throttled)
		return;

	/* remove from throttle queue */
	li_waitqueue_remove(&con->wrk->throttle_queue, &con->throttle.wqueue_elem);
	li_throttle_pool_release(con);

	con->throttle.con.rate = 0;
	con->throttle.con.magazine = 0;
	con->throttled = FALSE;
}

void li_throttle_cb(struct ev_loop *loop, ev_timer *w, int revents) {
	liWaitQueueElem *wqe;
	liThrottlePool *pool;
	liConnection *con;
	liWorker *wrk;
	ev_tstamp now;
	guint supply;

	UNUSED(revents);

	wrk = w->data;
	now = ev_now(loop);

	while (NULL != (wqe = li_waitqueue_pop(&wrk->throttle_queue))) {
		con = wqe->data;

		if (con->throttle.pool.ptr) {
			/* throttled by pool */
			pool = con->throttle.pool.ptr;

			li_throttle_pool_rearm(wrk, pool);

			if (con->throttle.con.rate) {
				supply = MIN(con->throttle.pool.magazine, con->throttle.con.rate * THROTTLE_GRANULARITY);
				con->throttle.con.magazine += supply;
				con->throttle.pool.magazine -= supply;
			} else {
				con->throttle.con.magazine += con->throttle.pool.magazine;
				con->throttle.pool.magazine = 0;
			}
		/* TODO: throttled by ip */
		} else {
			/* throttled by connection */
			if (con->throttle.con.magazine <= con->throttle.con.rate * THROTTLE_GRANULARITY * 4)
				con->throttle.con.magazine += con->throttle.con.rate * THROTTLE_GRANULARITY;
		}

		li_ev_io_add_events(loop, &con->sock_watcher, EV_WRITE);
	}

	li_waitqueue_update(&wrk->throttle_queue);
}

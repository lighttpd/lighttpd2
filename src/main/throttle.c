/*
 * Implemented with token bucket algorithm.
 * On average, the rate of bytes/s never exceeds the specified limit
 * but allows small bursts of previously unused bandwidth (max rate*2 for 1 second).
 */

#include <lighttpd/base.h>

liThrottlePool *li_throttle_pool_new(liServer *srv, GString *name, guint rate) {
	liThrottlePool *pool;
	guint i;

	g_mutex_lock(srv->action_mutex);

	/* check if we already have a pool with that name */
	for (i = 0; i < srv->throttle_pools->len; i++) {
		pool = g_array_index(srv->throttle_pools, liThrottlePool*, i);

		if (g_string_equal(pool->name, name)) {
			g_atomic_int_inc(&pool->refcount);
			g_mutex_unlock(srv->action_mutex);
			g_string_free(name, TRUE);
			return pool;
		}
	}

	if (rate == 0) {
		g_mutex_unlock(srv->action_mutex);
		return NULL;
	}

	pool = g_slice_new0(liThrottlePool);
	pool->rate = rate;
	pool->name = name;
	pool->last_rearm = THROTTLE_EVTSTAMP_TO_GINT(ev_time());
	pool->refcount = 1;

	/*
	 * We if we are not in LI_SERVER_INIT state, we can initialize the queues directly.
	 * Otherwise they'll get initialized in the worker prepare callback.
	 */
	if (g_atomic_int_get(&srv->state) != LI_SERVER_INIT) {
		pool->worker_magazine = g_new0(gint, srv->worker_count);
		pool->worker_last_rearm = g_new0(gint, srv->worker_count);
		pool->worker_num_cons_queued = g_new0(gint, srv->worker_count);
		pool->worker_queues = g_new0(GQueue*, srv->worker_count);

		for (i = 0; i < srv->worker_count; i++) {
			pool->worker_queues[i] = g_queue_new();
			pool->worker_last_rearm[i] = pool->last_rearm;
		}
	}

	g_array_append_val(srv->throttle_pools, pool);

	g_mutex_unlock(srv->action_mutex);

	return pool;
}

void li_throttle_pool_free(liServer *srv, liThrottlePool *pool) {
	guint i;

	if (!g_atomic_int_dec_and_test(&pool->refcount))
		return;

	g_mutex_lock(srv->action_mutex);
	for (i = 0; i < srv->throttle_pools->len; i++) {
		if (pool == g_array_index(srv->throttle_pools, liThrottlePool*, i)) {
			g_array_remove_index_fast(srv->throttle_pools, i);
			break;
		}
	}
	g_mutex_unlock(srv->action_mutex);

	if (pool->worker_queues) {
		for (i = 0; i < srv->worker_count; i++) {
			g_queue_free(pool->worker_queues[i]);
		}

		g_free(pool->worker_magazine);
		g_free(pool->worker_last_rearm);
		g_free(pool->worker_num_cons_queued);
		g_free(pool->worker_queues);
	}

	g_string_free(pool->name, TRUE);
	g_slice_free(liThrottlePool, pool);
}

void li_throttle_pool_acquire(liVRequest *vr, liThrottlePool *pool) {
	if (vr->throttle.pool.ptr == pool)
		return;

	g_atomic_int_inc(&pool->refcount);

	if (vr->throttle.pool.ptr != NULL) {
		/* already in a different pool */
		li_throttle_pool_release(vr);
	}

	vr->throttle.pool.ptr = pool;
	vr->throttled = TRUE;
}

void li_throttle_pool_release(liVRequest *vr) {
	if (vr->throttle.pool.ptr == NULL)
		return;

	g_atomic_int_add(&vr->throttle.pool.ptr->refcount, -1);

	if (vr->throttle.pool.queue) {
		g_queue_unlink(vr->throttle.pool.queue, &vr->throttle.pool.lnk);
		vr->throttle.pool.queue = NULL;
		g_atomic_int_add(&vr->throttle.pool.ptr->worker_num_cons_queued[vr->wrk->ndx], -1);
	}

	/* give back bandwidth */
	vr->throttle.pool.magazine = 0;
	vr->throttle.pool.ptr = NULL;
}

static void li_throttle_pool_rearm(liWorker *wrk, liThrottlePool *pool) {
	gint time_diff, supply, num_cons, magazine;
	guint i;
	GQueue *queue;
	GList *lnk, *lnk_next;
	guint now = THROTTLE_EVTSTAMP_TO_GINT(CUR_TS(wrk));

	if (now - pool->worker_last_rearm[wrk->ndx] < THROTTLE_GRANULARITY)
		return;

	/* milliseconds since last global rearm */
	time_diff = now - g_atomic_int_get(&pool->last_rearm);
	/* check if we have to rearm any magazines */
	if (time_diff >= THROTTLE_GRANULARITY) {
		/* spinlock while we rearm the magazines */
		if (g_atomic_int_compare_and_exchange(&pool->rearming, 0, 1)) {
			gint worker_num_cons[wrk->srv->worker_count];

			/* calculate total cons, take a safe "snapshot" */
			num_cons = 0;
			for (i = 0; i < wrk->srv->worker_count; i++) {
				worker_num_cons[i] = g_atomic_int_get(&pool->worker_num_cons_queued[i]);
				num_cons += worker_num_cons[i];
			}

			/* rearm the worker magazines */
			supply = ((pool->rate / 1000) * MIN(time_diff, 2000)) / num_cons;

			for (i = 0; i < wrk->srv->worker_count; i++) {
				if (worker_num_cons[i] == 0)
					continue;

				g_atomic_int_add(&pool->worker_magazine[i], supply * worker_num_cons[i]);
			}

			g_atomic_int_set(&pool->last_rearm, now);
			g_atomic_int_set(&pool->rearming, 0);
		}  else {
			/* wait for pool rearm to finish */
			while (g_atomic_int_get(&pool->rearming) == 1) { }
		}
	}


	/* select current queue */
	queue = pool->worker_queues[wrk->ndx];
	if (queue->length) {
		g_atomic_int_set(&pool->worker_num_cons_queued[wrk->ndx], 0);

		magazine = g_atomic_int_get(&pool->worker_magazine[wrk->ndx]);
		g_atomic_int_add(&pool->worker_magazine[wrk->ndx], -magazine);
		supply = magazine / queue->length;

		/* rearm connections */
		for (lnk = g_queue_peek_head_link(queue); lnk != NULL; lnk = lnk_next) {
			((liVRequest*)lnk->data)->throttle.pool.magazine += supply;
			((liVRequest*)lnk->data)->throttle.pool.queue = NULL;
			lnk_next = lnk->next;
			lnk->next = NULL;
			lnk->prev = NULL;
		}

		/* clear current connection queue */
		g_queue_init(queue);
	}

	pool->worker_last_rearm[wrk->ndx] = now;
}

void li_throttle_reset(liVRequest *vr) {
	if (!vr->throttled)
		return;

	/* remove from throttle queue */
	li_waitqueue_remove(&vr->wrk->throttle_queue, &vr->throttle.wqueue_elem);
	li_throttle_pool_release(vr);

	vr->throttle.con.rate = 0;
	vr->throttle.magazine = 0;
	vr->throttled = FALSE;
}

void li_throttle_cb(liWaitQueue *wq, gpointer data) {
	liWaitQueueElem *wqe;
	liThrottlePool *pool;
	liVRequest *vr;
	liWorker *wrk;
	ev_tstamp now;
	gint supply;

	wrk = data;
	now = ev_now(wrk->loop);

	while (NULL != (wqe = li_waitqueue_pop(wq))) {
		vr = wqe->data;

		if (vr->throttle.pool.ptr) {
			/* throttled by pool */
			pool = vr->throttle.pool.ptr;

			li_throttle_pool_rearm(wrk, pool);

			if (vr->throttle.con.rate) {
				supply = MIN(vr->throttle.pool.magazine, vr->throttle.con.rate / 1000 * THROTTLE_GRANULARITY);
				vr->throttle.magazine += supply;
				vr->throttle.pool.magazine -= supply;
			} else {
				vr->throttle.magazine += vr->throttle.pool.magazine;
				vr->throttle.pool.magazine = 0;
			}
		/* TODO: throttled by ip */
		} else {
			/* throttled by connection */
			if (vr->throttle.magazine <= vr->throttle.con.rate / 1000 * THROTTLE_GRANULARITY * 4)
				vr->throttle.magazine += vr->throttle.con.rate / 1000 * THROTTLE_GRANULARITY;
		}

		vr->coninfo->callbacks->handle_check_io(vr);
	}

	li_waitqueue_update(wq);
}

void li_throttle_update(liVRequest *vr, goffset transferred, goffset write_max) {
	vr->throttle.magazine -= transferred;

	/*g_print("%p wrote %"G_GINT64_FORMAT"/%"G_GINT64_FORMAT" bytes, mags: %d/%d, queued: %s\n", (void*)con,
	transferred, write_max, con->throttle.pool.magazine, con->throttle.con.magazine, con->throttle.pool.queued ? "yes":"no");*/

	if (vr->throttle.magazine <= 0) {
		li_waitqueue_push(&vr->wrk->throttle_queue, &vr->throttle.wqueue_elem);
	}

	if (vr->throttle.pool.ptr && vr->throttle.pool.magazine <= write_max && !vr->throttle.pool.queue) {
		liThrottlePool *pool = vr->throttle.pool.ptr;

		vr->throttle.pool.queue = pool->worker_queues[vr->wrk->ndx];
		g_queue_push_tail_link(vr->throttle.pool.queue, &vr->throttle.pool.lnk);
		g_atomic_int_inc(&pool->worker_num_cons_queued[vr->wrk->ndx]);
	}
}

/*
 * Implemented with token bucket algorithm.
 * On average, the rate of bytes/s never exceeds the specified limit
 * but allows small bursts of previously unused bandwidth (max rate*2 for 1 second).
 */

#include <lighttpd/base.h>

static void li_throttle_pool_rearm(liWorker *wrk, liThrottlePool *pool);

liThrottlePool *li_throttle_pool_new(liServer *srv, liThrottlePoolType type, gpointer param, guint rate) {
	liThrottlePool *pool;
	guint i;

	g_mutex_lock(srv->action_mutex);

	if (type == LI_THROTTLE_POOL_NAME) {
		/* named pool */
		GString *name = param;
		/* check if we already have a pool with that name */
		for (i = 0; i < srv->throttle_pools->len; i++) {
			pool = g_array_index(srv->throttle_pools, liThrottlePool*, i);

			if (g_string_equal(pool->data.name, name)) {
				g_atomic_int_inc(&pool->refcount);
				g_mutex_unlock(srv->action_mutex);
				g_string_free(name, TRUE);
				return pool;
			}
		}
	} else {
		/* IP address pool */
		liSocketAddress *remote_addr = param;

		if (remote_addr->addr->plain.sa_family == AF_INET)
			pool = li_radixtree_lookup_exact(srv->throttle_ip_pools, &remote_addr->addr->ipv4.sin_addr.s_addr, 32);
		else
			pool = li_radixtree_lookup_exact(srv->throttle_ip_pools, &remote_addr->addr->ipv6.sin6_addr.s6_addr, 128);

		if (pool) {
			g_mutex_unlock(srv->action_mutex);
			return pool;
		}
	}

	if (rate == 0) {
		g_mutex_unlock(srv->action_mutex);
		return NULL;
	}

	pool = g_slice_new0(liThrottlePool);
	pool->type = type;
	pool->rate = rate;
	pool->last_rearm = THROTTLE_EVTSTAMP_TO_GINT(ev_time());

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

	if (type == LI_THROTTLE_POOL_NAME) {
		pool->refcount = 1;
		pool->data.name = param;
		g_array_append_val(srv->throttle_pools, pool);
	} else {
		liSocketAddress *remote_addr = param;
		pool->data.addr = li_sockaddr_dup(*remote_addr);

		if (remote_addr->addr->plain.sa_family == AF_INET)
			li_radixtree_insert(srv->throttle_ip_pools, &remote_addr->addr->ipv4.sin_addr.s_addr, 32, pool);
		else
			li_radixtree_insert(srv->throttle_ip_pools, &remote_addr->addr->ipv6.sin6_addr.s6_addr, 128, pool);
	}

	g_mutex_unlock(srv->action_mutex);

	return pool;
}

void li_throttle_pool_free(liServer *srv, liThrottlePool *pool) {
	guint i;

	if (!g_atomic_int_dec_and_test(&pool->refcount))
		return;

	g_mutex_lock(srv->action_mutex);

	if (pool->type == LI_THROTTLE_POOL_NAME) {
		for (i = 0; i < srv->throttle_pools->len; i++) {
			if (pool == g_array_index(srv->throttle_pools, liThrottlePool*, i)) {
				g_array_remove_index_fast(srv->throttle_pools, i);
				break;
			}
		}

		g_string_free(pool->data.name, TRUE);
	} else {
		if (pool->data.addr.addr->plain.sa_family == AF_INET)
			li_radixtree_remove(srv->throttle_ip_pools, &pool->data.addr.addr->ipv4.sin_addr.s_addr, 32);
		else
			li_radixtree_remove(srv->throttle_ip_pools, &pool->data.addr.addr->ipv6.sin6_addr.s6_addr, 128);
		li_sockaddr_clear(&pool->data.addr);
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

	g_slice_free(liThrottlePool, pool);
}

void li_throttle_pool_acquire(liVRequest *vr, liThrottlePool *pool) {
	/* already in this pool */
	if (vr->throttle.pool.ptr == pool || vr->throttle.ip.ptr == pool)
		return;

	g_atomic_int_inc(&pool->refcount);

	if (pool->type == LI_THROTTLE_POOL_NAME) {
		if (vr->throttle.pool.ptr != NULL) {
			/* already in a different pool */
			li_throttle_pool_release(vr, vr->throttle.pool.ptr);
		}

		vr->throttle.pool.ptr = pool;
	} else {
		vr->throttle.ip.ptr = pool;
	}

	vr->throttled = TRUE;

	li_throttle_pool_rearm(vr->wrk, pool);
	li_throttle_update(vr, 0, 0);
}

void li_throttle_pool_release(liVRequest *vr, liThrottlePool *pool) {
	if (pool->type == LI_THROTTLE_POOL_NAME) {
		if (vr->throttle.pool.queue) {
			g_atomic_int_add(&pool->worker_num_cons_queued[vr->wrk->ndx], -1);
			g_queue_unlink(vr->throttle.pool.queue, &vr->throttle.pool.lnk);
			vr->throttle.pool.queue = NULL;
		}

		vr->throttle.pool.magazine = 0;
		vr->throttle.pool.ptr = NULL;
	} else {
		if (vr->throttle.ip.queue) {
			g_atomic_int_add(&pool->worker_num_cons_queued[vr->wrk->ndx], -1);
			g_queue_unlink(vr->throttle.ip.queue, &vr->throttle.ip.lnk);
			vr->throttle.ip.queue = NULL;
		}

		vr->throttle.ip.magazine = 0;
		vr->throttle.ip.ptr = NULL;
	}

	li_throttle_pool_free(vr->wrk->srv, pool);
}

static void li_throttle_pool_rearm(liWorker *wrk, liThrottlePool *pool) {
	gint time_diff, supply, num_cons, magazine;
	guint i;
	GQueue *queue;
	GList *lnk, *lnk_next;
	guint now = THROTTLE_EVTSTAMP_TO_GINT(CUR_TS(wrk));

	time_diff = now - pool->worker_last_rearm[wrk->ndx];
	/* overflow after 31 days... */
	if (G_UNLIKELY(time_diff < 0))
		time_diff = 1000;

	if (G_LIKELY(time_diff < THROTTLE_GRANULARITY) && G_LIKELY(time_diff > 0))
		return;

	/* milliseconds since last global rearm */
	time_diff = now - g_atomic_int_get(&pool->last_rearm);
	if (G_UNLIKELY(time_diff < 0))
		time_diff = 1000;

	/* check if we have to rearm any magazines */
	if (G_UNLIKELY(time_diff >= THROTTLE_GRANULARITY)) {
		/* spinlock while we rearm the magazines */
		if (g_atomic_int_compare_and_exchange(&pool->rearming, 0, 1)) {
			gint worker_num_cons[wrk->srv->worker_count];

			/* calculate total cons, take a safe "snapshot" */
			num_cons = 0;
			for (i = 0; i < wrk->srv->worker_count; i++) {
				worker_num_cons[i] = g_atomic_int_get(&pool->worker_num_cons_queued[i]);
				num_cons += worker_num_cons[i];
			}

			if (num_cons) {
				/* rearm the worker magazines */
				supply = ((pool->rate / 1000) * MIN(time_diff, 1000)) / num_cons;
				for (i = 0; i < wrk->srv->worker_count; i++) {
					if (worker_num_cons[i] == 0)
						continue;

					g_atomic_int_add(&pool->worker_magazine[i], supply * worker_num_cons[i]);
				}
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
			if (pool->type == LI_THROTTLE_POOL_NAME) {
				((liVRequest*)lnk->data)->throttle.pool.magazine += supply;
				((liVRequest*)lnk->data)->throttle.pool.queue = NULL;
			} else {
				((liVRequest*)lnk->data)->throttle.ip.magazine += supply;
				((liVRequest*)lnk->data)->throttle.ip.queue = NULL;
			}
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

	if (vr->throttle.pool.ptr)
		li_throttle_pool_release(vr, vr->throttle.pool.ptr);
	if (vr->throttle.ip.ptr)
		li_throttle_pool_release(vr, vr->throttle.ip.ptr);

	vr->throttle.pool.magazine = 0;
	vr->throttle.ip.magazine = 0;
	vr->throttle.con.rate = 0;
	vr->throttle.magazine = 0;
	vr->throttled = FALSE;
}

void li_throttle_cb(liWaitQueue *wq, gpointer data) {
	liWaitQueueElem *wqe;
	liVRequest *vr;
	liWorker *wrk;
	gint supply;

	wrk = data;

	while (NULL != (wqe = li_waitqueue_pop(wq))) {
		vr = wqe->data;

		if (vr->throttle.pool.ptr) {
			/* throttled by pool */
			li_throttle_pool_rearm(wrk, vr->throttle.pool.ptr);

			if (vr->throttle.ip.ptr) {
				/* throttled by pool+IP */

				li_throttle_pool_rearm(wrk, vr->throttle.ip.ptr);

				supply = MIN(vr->throttle.pool.magazine, vr->throttle.ip.magazine);

				if (vr->throttle.con.rate) {
					/* throttled by pool+IP+con */
					supply = MIN(supply, vr->throttle.con.rate / 1000 * THROTTLE_GRANULARITY);
				}

				vr->throttle.pool.magazine -= supply;
				vr->throttle.ip.magazine -= supply;
				vr->throttle.magazine += supply;
			} else if (vr->throttle.con.rate) {
				/* throttled by pool+con */
				supply = MIN(vr->throttle.pool.magazine, vr->throttle.con.rate / 1000 * THROTTLE_GRANULARITY);
				vr->throttle.magazine += supply;
				vr->throttle.pool.magazine -= supply;
			} else {
				vr->throttle.magazine += vr->throttle.pool.magazine;
				vr->throttle.pool.magazine = 0;
			}
		} else if (vr->throttle.ip.ptr) {
			/* throttled by IP */
			li_throttle_pool_rearm(wrk, vr->throttle.ip.ptr);

			if (vr->throttle.con.rate) {
				/* throttled by IP+con */
				supply = MIN(vr->throttle.ip.magazine, vr->throttle.con.rate / 1000 * THROTTLE_GRANULARITY);
				vr->throttle.magazine += supply;
				vr->throttle.ip.magazine -= supply;
			} else {
				vr->throttle.magazine += vr->throttle.ip.magazine;
				vr->throttle.ip.magazine = 0;
			}
		} else {
			/* throttled by connection */
			if (vr->throttle.magazine <= vr->throttle.con.rate)
				vr->throttle.magazine += vr->throttle.con.rate / 1000 * THROTTLE_GRANULARITY;
		}

		vr->coninfo->callbacks->handle_check_io(vr);
	}
	li_waitqueue_update(wq);
}

void li_throttle_update(liVRequest *vr, goffset transferred, goffset write_max) {
	vr->throttle.magazine -= transferred;

	if (vr->throttle.magazine <= 0) {
		li_waitqueue_push(&vr->wrk->throttle_queue, &vr->throttle.wqueue_elem);
	}

	/* queue in pool if necessary */
	if (vr->throttle.pool.ptr && vr->throttle.pool.magazine <= write_max && !vr->throttle.pool.queue) {
		liThrottlePool *pool = vr->throttle.pool.ptr;

		vr->throttle.pool.queue = pool->worker_queues[vr->wrk->ndx];
		g_queue_push_tail_link(vr->throttle.pool.queue, &vr->throttle.pool.lnk);
		g_atomic_int_inc(&pool->worker_num_cons_queued[vr->wrk->ndx]);
	}

	/* queue in IP pool if necessary */
	if (vr->throttle.ip.ptr && vr->throttle.ip.magazine <= write_max && !vr->throttle.ip.queue) {
		liThrottlePool *pool = vr->throttle.ip.ptr;

		vr->throttle.ip.queue = pool->worker_queues[vr->wrk->ndx];
		g_queue_push_tail_link(vr->throttle.ip.queue, &vr->throttle.ip.lnk);
		g_atomic_int_inc(&pool->worker_num_cons_queued[vr->wrk->ndx]);
	}
}

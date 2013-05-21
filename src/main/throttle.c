/*
 * Implemented with token bucket algorithm.
 */

#include <lighttpd/throttle.h>
#include <math.h>

/* max amount of bytes we release in one query */
#define THROTTLE_MAX_STEP (64*1024)
/* even if the magazine is empty release "overload" bytes to get requests started */
#define THROTTLE_OVERLOAD (8*1024)

/* debug */
#if 0
#define STRINGIFY(x) #x
#define _STRINGIFY(x) STRINGIFY(x)
#define throttle_debug(...) fprintf(stderr, "throttle.c:" _STRINGIFY(__LINE__) ": " __VA_ARGS__)
#else
#define throttle_debug(...) do { } while (0)
#endif

/* rates all in bytes/sec */

typedef struct liThrottlePoolState liThrottlePoolState;
typedef struct liThrottlePoolWorkerState liThrottlePoolWorkerState;

struct liThrottlePoolState {
	liThrottlePool *pool;
	GList pool_link;

	 /* currently available for use */
	gint magazine;
};

struct liThrottleState {
	gint magazine; /* currently available for use */

	guint interested;
	liWaitQueueElem wqueue_elem;
	liThrottleNotifyCB notify_callback;

	/* max values for this single state */
	gint single_magazine;
	guint single_rate, single_burst;
	guint single_last_rearm;

	/* shared pools */
	GPtrArray *pools; /* <liThrottlePoolState> */
};

struct liThrottlePoolWorkerState {
	gint magazine;
	guint last_rearm;
	guint connections; /* waiting.length; needed for atomic access */
	GQueue waiting; /* <liThrottlePoolState.pool_link> waiting to get filled */
};

struct liThrottlePool {
	int refcount;

	GMutex *rearm_mutex;
	guint rate, burst;
	guint last_rearm;

	liThrottlePoolWorkerState *workers;
};

static guint msec_timestamp(li_tstamp now) {
	return (1000u * (guint64) floor(now)) + (guint64)(1000.0 * fmod(now, 1.0));
}

static void S_throttle_pool_rearm_workers(liThrottlePool *pool, guint worker_count, guint time_diff) {
	guint i;
	gint64 connections = 0;
	gint64 wrk_connections[worker_count];
	gint64 fill;

	for (i = 0; i < worker_count; ++i) {
		wrk_connections[i] = g_atomic_int_get((gint*) &pool->workers[i].connections);
		connections += wrk_connections[i];
	}

	if (0 == connections) return;

	time_diff = MIN(time_diff, 1000);

	fill = MIN((guint64) pool->burst, ((guint64) pool->rate * time_diff) / 1000u);

	throttle_debug("rearm workers: refill %i after %u (or more) msecs (rate %u, burst %u)\n",
		(guint) fill, (guint) time_diff, pool->rate, pool->burst);

	for (i = 0; i < worker_count; ++i) {
		gint wrk_fill;
		if (0 == wrk_connections[i]) continue;
		wrk_fill = (fill * wrk_connections[i]) / connections;
		throttle_debug("rearm worker %u: refill %u\n", i, wrk_fill);
		g_atomic_int_add(&pool->workers[i].magazine, wrk_fill);
	}
}

static void throttle_pool_rearm(liWorker *wrk, liThrottlePool *pool, guint now) {
	liThrottlePoolWorkerState *wpool = &pool->workers[wrk->ndx];
	guint last = g_atomic_int_get((gint*) &pool->last_rearm);
	guint time_diff = now - last;

	if (G_UNLIKELY(time_diff >= THROTTLE_GRANULARITY)) {
		g_mutex_lock(pool->rearm_mutex);
			/* check again */
			last = g_atomic_int_get((gint*) &pool->last_rearm);
			time_diff = now - last;
			if (G_LIKELY(time_diff >= THROTTLE_GRANULARITY)) {
				S_throttle_pool_rearm_workers(pool, wrk->srv->worker_count, time_diff);
				g_atomic_int_set((gint*) &pool->last_rearm, now);
			}
		g_mutex_unlock(pool->rearm_mutex);
	}

	if (G_UNLIKELY(wpool->last_rearm < last)) {
		/* distribute wpool->magazine */
		GList *lnk;
		guint connections = wpool->connections;
		gint magazine = g_atomic_int_get(&wpool->magazine);
		gint supply = magazine / connections;
		g_atomic_int_add(&wpool->magazine, -supply * connections);
		wpool->last_rearm = now;

		throttle_debug("throttle_pool_rearm: distribute supply %i on each of %i connections\n",
			supply, connections);

		if (0 == supply) return;

		g_atomic_int_set((gint*) &wpool->connections, 0);
		while (NULL != (lnk = g_queue_pop_head_link(&wpool->waiting))) {
			liThrottlePoolState *pstate = LI_CONTAINER_OF(lnk, liThrottlePoolState, pool_link);
			pstate->magazine += supply;
			lnk->data = NULL;
		}
	}
}

static void throttle_register(liThrottlePoolWorkerState *pwstate, liThrottlePoolState *pstate) {
	if (NULL == pstate->pool_link.data) {
		g_queue_push_tail_link(&pwstate->waiting, &pstate->pool_link);
		pstate->pool_link.data = &pwstate->waiting;
		g_atomic_int_inc((gint*) &pwstate->connections);
	}
}
static void throttle_unregister(liThrottlePoolWorkerState *pwstate, liThrottlePoolState *pstate) {
	if (NULL != pstate->pool_link.data) {
		g_queue_unlink(&pwstate->waiting, &pstate->pool_link);
		pstate->pool_link.data = NULL;
		g_atomic_int_add((gint*) &pwstate->connections, -1);
	}
}

guint li_throttle_query(liWorker *wrk, liThrottleState *state, guint interested, liThrottleNotifyCB notify_callback, gpointer data) {
	guint now = msec_timestamp(li_cur_ts(wrk));
	gint fill, pool_fill;
	guint i, len;

	if (NULL == state) return interested;

	state->notify_callback = NULL;
	state->wqueue_elem.data = NULL;

	throttle_debug("li_throttle_query[%u]: interested %i, magazine %i\n", now, interested, state->magazine);

	if (interested > THROTTLE_MAX_STEP) interested = THROTTLE_MAX_STEP;

	if ((gint) interested <= state->magazine + THROTTLE_OVERLOAD) return interested;

	/* also try to balance negative magazine */
	fill = interested - state->magazine;
	if (state->single_rate != 0) {
		if (now - state->single_last_rearm >= THROTTLE_GRANULARITY) {
			guint single_fill = ((guint64) state->single_rate) * 1000u / (now - state->single_last_rearm);
			state->single_last_rearm = now;
			if (state->single_burst - state->single_magazine < single_fill) {
				state->single_magazine = state->single_burst;
			} else {
				state->single_magazine += single_fill;
			}
		}
		if (fill > state->single_magazine) fill = state->single_magazine;
		throttle_debug("single_magazine: %i\n", state->single_magazine);
	}

	/* pool_fill <= fill in the loop */
	pool_fill = fill;
	for (i = 0, len = state->pools->len; i < len; ++i) {
		liThrottlePoolState *pstate = g_ptr_array_index(state->pools, i);
		liThrottlePool *pool = pstate->pool;
		liThrottlePoolWorkerState *pwstate = &pool->workers[wrk->ndx];
		if (fill > pstate->magazine) {
			throttle_register(pwstate, pstate);
			throttle_pool_rearm(wrk, pool, now);
			if (fill > pstate->magazine) {
				throttle_register(pwstate, pstate);
				if (pool_fill > pstate->magazine) {
					pool_fill = pstate->magazine;
				}
			}
		}
		throttle_debug("pool %i magazine: %i\n", i, state->single_magazine);
	}

	throttle_debug("query refill: %i\n", pool_fill);

	if (pool_fill > 0) {
		if (state->single_rate != 0) {
			state->single_magazine -= pool_fill;
		}
		for (i = 0, len = state->pools->len; i < len; ++i) {
			liThrottlePoolState *pstate = g_ptr_array_index(state->pools, i);
			pstate->magazine -= pool_fill;
		}
		state->magazine += pool_fill;
	}

	if (state->magazine + THROTTLE_OVERLOAD <= 0) {
		throttle_debug("query queueing\n");
		state->wqueue_elem.data = data;
		state->notify_callback = notify_callback;
		state->interested = interested;
		if (!state->wqueue_elem.queued) {
			li_waitqueue_push(&wrk->throttle_queue, &state->wqueue_elem);
		}
		return 0;
	}

	throttle_debug("query success: %i\n", state->magazine + THROTTLE_OVERLOAD);

	if ((gint) interested <= state->magazine + THROTTLE_OVERLOAD) return interested;
	return state->magazine + THROTTLE_OVERLOAD;
}

void li_throttle_update(liThrottleState *state, guint used) {
	state->magazine -= used;
}

void li_throttle_pool_acquire(liThrottlePool *pool) {
	assert(g_atomic_int_get(&pool->refcount) > 0);
	g_atomic_int_inc(&pool->refcount);
}

void li_throttle_pool_release(liThrottlePool *pool, liServer *srv) {
	assert(g_atomic_int_get(&pool->refcount) > 0);
	if (g_atomic_int_dec_and_test(&pool->refcount)) {
		g_mutex_free(pool->rearm_mutex);
		pool->rearm_mutex = NULL;
		if (NULL != pool->workers) {
			g_slice_free1(sizeof(liThrottlePoolWorkerState) * srv->worker_count, pool->workers);
			pool->workers = NULL;
		}
	}
}

gboolean li_throttle_add_pool(liWorker *wrk, liThrottleState *state, liThrottlePool *pool) {
	liThrottlePoolState *pstate;
	guint i, len;
	assert(NULL != wrk);
	assert(NULL != state);

	if (NULL == pool) return FALSE;
	for (i = 0, len = state->pools->len; i < len; ++i) {
		pstate = g_ptr_array_index(state->pools, i);
		if (pstate->pool == pool) return FALSE;
	}

	li_throttle_pool_acquire(pool);
	pstate = g_slice_new0(liThrottlePoolState);
	pstate->pool = pool;
	g_ptr_array_add(state->pools, pstate);

	return TRUE;
}

void li_throttle_remove_pool(liWorker *wrk, liThrottleState *state, liThrottlePool *pool) {
	guint i, len;
	assert(NULL != wrk);
	if (NULL == state || NULL == pool) return;

	for (i = 0, len = state->pools->len; i < len; ++i) {
		liThrottlePoolState *pstate = g_ptr_array_index(state->pools, i);
		if (pstate->pool == pool) {
			throttle_unregister(&pool->workers[wrk->ndx], pstate);
			g_ptr_array_remove_index_fast(state->pools, i);
			li_throttle_pool_release(pool, wrk->srv);
			g_slice_free(liThrottlePoolState, pstate);
			return;
		}
	}
}

liThrottleState* li_throttle_new() {
	liThrottleState *state = g_slice_new0(liThrottleState);
	state->pools = g_ptr_array_new();
	return state;
}

void li_throttle_set(liWorker *wrk, liThrottleState *state, guint rate, guint burst) {
	UNUSED(wrk);
	state->single_rate = rate;
	state->single_burst = burst;
	state->single_magazine = burst;
	state->single_last_rearm = msec_timestamp(li_cur_ts(wrk));
}

void li_throttle_free(liWorker *wrk, liThrottleState *state) {
	guint i, len;
	assert(NULL != wrk);

	if (NULL == state) return;

	for (i = 0, len = state->pools->len; i < len; ++i) {
		liThrottlePoolState *pstate = g_ptr_array_index(state->pools, i);
		throttle_unregister(&pstate->pool->workers[wrk->ndx], pstate);
		li_throttle_pool_release(pstate->pool, wrk->srv);
		g_slice_free(liThrottlePoolState, pstate);
	}
	g_ptr_array_free(state->pools, TRUE);

	li_waitqueue_remove(&wrk->throttle_queue, &state->wqueue_elem);
	g_slice_free(liThrottleState, state);
}

static void throttle_prepare(liServer *srv, gpointer data, gboolean aborted) {
	liThrottlePool *pool = data;

	if (!aborted) {
		guint i, len = srv->worker_count;

		pool->workers = g_slice_alloc0(sizeof(liThrottlePoolWorkerState) * len);
		for (i = 0; i < len; ++i) {
			liThrottlePoolWorkerState *pwstate = &pool->workers[i];
			pwstate->last_rearm = pool->last_rearm;
			pwstate->magazine = pool->burst / len;
		}
	}
	li_throttle_pool_release(pool, srv);
}

liThrottlePool* li_throttle_pool_new(liServer *srv, guint rate, guint burst) {
	liThrottlePool *pool = g_slice_new0(liThrottlePool);
	pool->refcount = 2; /* one for throttle_prepare() */
	pool->last_rearm = msec_timestamp(li_event_time());
	pool->rearm_mutex = g_mutex_new();
	pool->rate = rate;
	pool->burst = burst;
	li_server_register_prepare_cb(srv, throttle_prepare, pool);
	return pool;
}

void li_throttle_waitqueue_cb(liWaitQueue *wq, gpointer data) {
	liWaitQueueElem *wqe;
	UNUSED(data); /* should contain worker */

	throttle_debug("li_throttle_waitqueue_cb\n");

	while (NULL != (wqe = li_waitqueue_pop(wq))) {
		liThrottleState *state = LI_CONTAINER_OF(wqe, liThrottleState, wqueue_elem);
		liThrottleNotifyCB notify_callback = state->notify_callback;
		gpointer notify_data = wqe->data;

		if (NULL == notify_data || NULL == notify_callback || 0 == state->interested) continue;

		notify_callback(state, notify_data);
	}
	li_waitqueue_update(wq);
}

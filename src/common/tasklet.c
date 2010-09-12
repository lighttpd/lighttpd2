
#include <lighttpd/tasklet.h>

typedef struct liTasklet liTasklet;

struct liTaskletPool {
	GThreadPool *threadpool;

	struct ev_loop *loop;
	ev_async finished_watcher;
	GAsyncQueue *finished;

	int threads;

	/* -1: running finished_watcher_cb, do not delete
	 *  0: standard, do not delete, can delete
	 *  1: running finished_watcher_cb, delete in finished_watcher_cb
	 */
	int delete_later;
};

struct liTasklet {
	liTaskletRunCB run_cb;
	liTaskletFinishedCB finished_cb;
	gpointer data;
};

static void finished_watcher_cb(struct ev_loop *loop, ev_async *w, int revents) {
	liTaskletPool *pool = w->data;
	liTasklet *t;
	UNUSED(loop);
	UNUSED(revents);

	pool->delete_later = -1;

	while (NULL != (t = g_async_queue_try_pop(pool->finished))) {
		t->finished_cb(t->data);

		g_slice_free(liTasklet, t);

		if (1 == pool->delete_later) {
			g_slice_free(liTaskletPool, pool);
			return;
		}
	}

	pool->delete_later = 0;
}

static void run_tasklet(gpointer data, gpointer userdata) {
	liTaskletPool *pool = userdata;
	liTasklet *t = data;

	t->run_cb(t->data);
	g_async_queue_push(pool->finished, t);
	ev_async_send(pool->loop, &pool->finished_watcher);
}

liTaskletPool* li_tasklet_pool_new(struct ev_loop *loop, gint threads) {
	liTaskletPool *pool = g_slice_new0(liTaskletPool);

	pool->loop = loop;

	ev_init(&pool->finished_watcher, finished_watcher_cb);
	pool->finished_watcher.data = pool;
	ev_async_start(pool->loop, &pool->finished_watcher);
	ev_unref(pool->loop);

	pool->finished = g_async_queue_new();

	li_tasklet_pool_set_threads(pool, threads);

	return pool;
}

void li_tasklet_pool_free(liTaskletPool *pool) {
	liTasklet *t;

	if (!pool) return;

	li_tasklet_pool_set_threads(pool, 0);

	while (NULL != (t = g_async_queue_try_pop(pool->finished))) {
		t->finished_cb(t->data);
	}
	g_async_queue_unref(pool->finished);
	pool->finished = NULL;

	ev_ref(pool->loop);
	ev_async_stop(pool->loop, &pool->finished_watcher);

	if (-1 == pool->delete_later) {
		pool->delete_later = 1;
	} else {
		g_slice_free(liTaskletPool, pool);
	}
}

void li_tasklet_pool_set_threads(liTaskletPool *pool, gint threads) {
	if (threads < 0) threads = -1;
	if (pool->threads == threads) return;

	if (NULL != pool->threadpool) {
		if (pool->threads > 0 && threads > 0) {
			/* pool was exclusive, stays exlusive. just change number of threads */
			g_thread_pool_set_max_threads(pool->threadpool, threads, NULL);
			pool->threads = g_thread_pool_get_num_threads(pool->threadpool);
			/* as we already had exclusive threads running, pool->threads should be > 0 */
			return;
		}

		/* stop old pool */
		g_thread_pool_free(pool->threadpool, FALSE, TRUE);
		pool->threadpool = NULL;
	}

	if (threads != 0) {
		pool->threadpool = g_thread_pool_new(run_tasklet, pool, threads, (threads > 0), NULL);
		if (threads > 0) { /* exclusive pool, see how many threads we got */
			threads = g_thread_pool_get_num_threads(pool->threadpool);
			if (threads == 0) { /* couldn't get exlusive threads, share threads instead */
				g_thread_pool_free(pool->threadpool, FALSE, TRUE);
				pool->threadpool = g_thread_pool_new(run_tasklet, pool, -1, FALSE, NULL);
				threads = -1;
			}
		}
	}

	pool->threads = threads;
}

gint li_tasklet_pool_get_threads(liTaskletPool *pool) {
	return pool->threads;
}

void li_tasklet_push(liTaskletPool* pool, liTaskletRunCB run, liTaskletFinishedCB finished, gpointer data) {
	liTasklet *t = g_slice_new0(liTasklet);
	t->run_cb = run;
	t->finished_cb = finished;
	t->data = data;

	if (NULL != pool->threadpool) {
		g_thread_pool_push(pool->threadpool, t, NULL);
	} else {
		run_tasklet(t, pool);
	}
}


#include <lighttpd/backends.h>

typedef struct liBackendConnection_p liBackendConnection_p;
typedef struct liBackendWorkerPool liBackendWorkerPool;
typedef struct liBackendPool_p liBackendPool_p;

struct liBackendWait {
	li_tstamp ts_started;

	/* 3 different states:
	 *  - con != NULL: connection associated (may need to move between threads/...)
	 *  - failed = TRUE: backend is down
	 *  - queued in wait_queue with link
	 */

	liBackendConnection_p *con;
	GList wait_queue_link; /* link in wait_queue (either pool or wpool) */

	gboolean failed;
	liVRequest *vr;
	liJobRef *vr_ref;
};

/* members marked with "[pool]" are protected by the pool lock, the others belong to the worker */
struct liBackendConnection_p {
	liBackendConnection public;

	liBackendPool_p *pool;

	gint ndx; /* [pool] index in the worker pool (worker ? worker : worker_next )->connections */
	gboolean active; /* [pool] */
	gint requests;

	liWaitQueueElem timeout_elem; /* ([pool]) idle or connect */

	/* if != NULL the connection is reserved by a vrequest,
	 *   and waiting to get transferred to the correct worker
	 */
	liBackendWait *wait; /* [pool] */

	/* worker != NULL: connection belongs to worker
	 *   worker_next != NULL: worker should release connection, and wakeup worker_next
	 * worker == NULL:
	 *   worker_next must be != NULL: worker_next is waiting to be woken up, but
	 *     if the connection isn't reserved anymore (wait == NULL) another worker can
	 *     steal it
	 * changing worker leads to the corresponding attach/detach callbacks
	 */
	liWorker *worker, *worker_next; /* [pool] */
};

/* members marked with "[pool]" are protected by the pool lock, the others belong to the worker */
struct liBackendWorkerPool {
	liBackendPool_p *pool;
	liWorker *wrk;

	liEventAsync wakeup;
	guint active, reserved, idle, pending; /* [pool] connection counts */
	/* attached connections (con->worker != NULL) may only be removed (and added) by the owning worker */
	GPtrArray *connections; /* [pool] <liBackendConnection_p> ordered: active, reserved, idle */
	liWaitQueue idle_queue; /* <liBackendConnection_p> */
	liWaitQueue connect_queue; /* <liBackendConnection_p> pending connects */

	/* waiting vrequests: per worker queue if there is no connection limit (<= 0) */
	GQueue wait_queue; /* <liBackendWait> */
	liEventTimer wait_queue_timer;

	gboolean initialized; /* [pool] only interesting if pool->initialized is false */
};

struct liBackendPool_p {
	liBackendPool public;

	GMutex *lock;
	liBackendWorkerPool *worker_pools;

	guint active, reserved, idle, pending, total; /* [pool] connection counts. */

	/* waiting vrequests: global queue if connection limit > 0 is used */
	GQueue wait_queue; /* <liBackendWait> */
	/* ^^ should use timer in worker-pool? */

	li_tstamp ts_disabled_till;

	gboolean initialized, shutdown;
};

static void S_backend_pool_distribute(liBackendPool_p *pool, liWorker *wrk);
static void backend_con_watch_for_close_cb(liEventBase *watcher, int events);

static void _call_thread_cb(liBackendConnectionThreadCB cb, liBackendPool *bpool, liWorker *wrk, liBackendConnection *bcon) {
	if (cb) cb(bpool, wrk, bcon);
}

#define BACKEND_THREAD_CB(name, pool, wrk, con) _call_thread_cb(pool->public.config->callbacks->name##_cb, &pool->public, wrk, &con->public)

static void S_backend_pool_worker_remove_con(liBackendPool_p *pool, liBackendConnection_p *con) {
	liWorker *cur = con->worker != NULL ? con->worker : con->worker_next;
	liBackendWorkerPool *wpool = &pool->worker_pools[cur->ndx];

	gint ndx = con->ndx;

	gint last_active_ndx = wpool->active - 1;
	gint last_reserved_ndx = last_active_ndx + wpool->reserved;
	gint last_idle_ndx = last_reserved_ndx + wpool->idle;

	LI_FORCE_ASSERT(con->ndx >= 0 && (guint)con->ndx < wpool->connections->len);
	LI_FORCE_ASSERT(g_ptr_array_index(wpool->connections, con->ndx) == con);
	LI_FORCE_ASSERT(last_idle_ndx == (gint) wpool->connections->len - 1);

	if (ndx <= last_active_ndx) {
		--wpool->active;
		--pool->active;
	} else if (ndx <= last_reserved_ndx) {
		--wpool->reserved;
		--pool->reserved;
	} else {
		--wpool->idle;
		--pool->idle;
	}
	--pool->total;

	if (ndx < last_active_ndx) {
		liBackendConnection_p *move = g_ptr_array_index(wpool->connections, last_active_ndx);
		g_ptr_array_index(wpool->connections, ndx) = move;
		move->ndx = ndx;
		ndx = last_active_ndx;
	}
	if (ndx < last_reserved_ndx) {
		liBackendConnection_p *move = g_ptr_array_index(wpool->connections, last_reserved_ndx);
		g_ptr_array_index(wpool->connections, ndx) = move;
		move->ndx = ndx;
		ndx = last_reserved_ndx;
	}
	if (ndx < last_idle_ndx) {
		liBackendConnection_p *move = g_ptr_array_index(wpool->connections, last_idle_ndx);
		g_ptr_array_index(wpool->connections, ndx) = move;
		move->ndx = ndx;
		ndx = last_idle_ndx;
	}
	g_ptr_array_index(wpool->connections, last_idle_ndx) = NULL;
	g_ptr_array_set_size(wpool->connections, last_idle_ndx);
	con->ndx = -1;
}

static void S_backend_pool_worker_insert_con(liBackendPool_p *pool, liWorker *wrk, liBackendConnection_p *con) {
	liWorker *cur = con->worker != NULL ? con->worker : con->worker_next;
	liBackendWorkerPool *wpool;
	gint min_ndx, max_ndx;

	if (wrk == NULL) wrk = cur;

	if (-1 != con->ndx && wrk != cur) {
		S_backend_pool_worker_remove_con(pool, con);
	}

	wpool = &pool->worker_pools[wrk->ndx];
	if (-1 == con->ndx) {
		con->ndx = wpool->connections->len;
		g_ptr_array_add(wpool->connections, con);
		++pool->total;
	} else if ((guint) con->ndx < wpool->active) {
		--wpool->active;
		--pool->active;
	} else if ((guint) con->ndx < wpool->active + wpool->reserved) {
		--wpool->reserved;
		--pool->reserved;
	} else {
		--wpool->idle;
		--pool->idle;
	}

	if (con->active) {
		++wpool->active;
		++pool->active;

		min_ndx = 0;
		max_ndx = wpool->active - 1;
	} else if (NULL == con->worker || NULL != con->wait) {
		++wpool->reserved;
		++pool->reserved;

		min_ndx = wpool->active;
		max_ndx = wpool->active + wpool->reserved - 1;
	} else {
		++wpool->idle;
		++pool->idle;

		min_ndx = wpool->active + wpool->reserved;
		max_ndx = wpool->active + wpool->reserved + wpool->idle - 1;
	}

	if (con->ndx < min_ndx) {
		if ((guint) con->ndx < wpool->active) {
			liBackendConnection_p *move = g_ptr_array_index(wpool->connections, wpool->active);
			g_ptr_array_index(wpool->connections, con->ndx) = move;
			move->ndx = con->ndx;
			con->ndx = wpool->active;
		}
		if (con->ndx < min_ndx && (guint) con->ndx < wpool->active + wpool->reserved) {
			liBackendConnection_p *move = g_ptr_array_index(wpool->connections, wpool->active + wpool->reserved);
			g_ptr_array_index(wpool->connections, con->ndx) = move;
			move->ndx = con->ndx;
			con->ndx = wpool->active + wpool->reserved;
		}
		g_ptr_array_index(wpool->connections, con->ndx) = con;
		LI_FORCE_ASSERT(con->ndx == min_ndx);
	} else if (con->ndx > max_ndx) {
		if ((guint) con->ndx > wpool->active + wpool->reserved - 1) {
			liBackendConnection_p *move = g_ptr_array_index(wpool->connections, wpool->active + wpool->reserved - 1);
			g_ptr_array_index(wpool->connections, con->ndx) = move;
			move->ndx = con->ndx;
			con->ndx = wpool->active + wpool->reserved - 1;
		}
		if (con->ndx > max_ndx && (guint) con->ndx > wpool->active - 1) {
			liBackendConnection_p *move = g_ptr_array_index(wpool->connections, wpool->active - 1);
			g_ptr_array_index(wpool->connections, con->ndx) = move;
			move->ndx = con->ndx;
			con->ndx = wpool->active - 1;
		}
		g_ptr_array_index(wpool->connections, con->ndx) = con;
		LI_FORCE_ASSERT(con->ndx == max_ndx);
	} else {
		LI_FORCE_ASSERT(con->ndx >= min_ndx && con->ndx <= max_ndx);
	}
}

static liBackendConnection_p* backend_connection_new(liBackendWorkerPool *wpool) {
	liBackendConnection_p *con = g_slice_new0(liBackendConnection_p);

	con->pool = wpool->pool;
	con->ndx = -1;
	con->worker = wpool->wrk;

	return con;
}

static void S_backend_pool_worker_insert_connected(liBackendWorkerPool *wpool, int fd) {
	liBackendConnection_p *con = backend_connection_new(wpool);
	liBackendPool_p *pool = wpool->pool;

	li_event_io_init(&wpool->wrk->loop, "backend connection", &con->public.watcher, NULL, fd, 0);
	li_event_set_keep_loop_alive(&con->public.watcher, FALSE);

	BACKEND_THREAD_CB(new, pool, wpool->wrk, con);

	if (pool->public.config->watch_for_close) {
		li_event_set_callback(&con->public.watcher, backend_con_watch_for_close_cb);
		li_event_io_set_events(&con->public.watcher, LI_EV_READ);
		li_event_start(&con->public.watcher);
	}
	li_waitqueue_push(&wpool->idle_queue, &con->timeout_elem);

	S_backend_pool_worker_insert_con(pool, NULL, con);
}

static void S_backend_pool_failed(liBackendWorkerPool *wpool) {
	liBackendPool_p *pool = wpool->pool;
	GList *elem;

	if (pool->public.config->disable_time > 0) {
		pool->ts_disabled_till = li_cur_ts(wpool->wrk) + pool->public.config->disable_time;
	}

	while (NULL != (elem = g_queue_pop_head_link(&pool->wait_queue))) {
		liBackendWait *bwait = LI_CONTAINER_OF(elem, liBackendWait, wait_queue_link);
		bwait->failed = TRUE;
		li_job_async(bwait->vr_ref);
	}

	for (guint i = 0, len = wpool->wrk->srv->worker_count; i < len; ++i) {
		liBackendWorkerPool *_wpool = &pool->worker_pools[i];
		while (NULL != (elem = g_queue_pop_head_link(&_wpool->wait_queue))) {
			liBackendWait *bwait = LI_CONTAINER_OF(elem, liBackendWait, wait_queue_link);
			bwait->failed = TRUE;
			li_job_async(bwait->vr_ref);
		}
	}
}

/* See http://www.cyberconf.org/~cynbe/ref/nonblocking-connects.html
 * for a discussion on async connects
 */

static void backend_pool_worker_connect_timeout(liWaitQueue *wq, gpointer data) {
	liBackendWorkerPool *wpool = data;
	liBackendPool_p *pool = wpool->pool;
	liWaitQueueElem *elem;
	const liBackendConfig *config = pool->public.config;
	liServer *srv = wpool->wrk->srv;

	g_mutex_lock(pool->lock);

	while (NULL != (elem = li_waitqueue_pop(wq))) {
		liBackendConnection_p *con = LI_CONTAINER_OF(elem, liBackendConnection_p, timeout_elem);
		li_event_clear(&con->public.watcher);
		close(li_event_io_fd(&con->public.watcher));
		g_slice_free(liBackendConnection_p, con);

		ERROR(srv, "Couldn't connect to '%s': timeout",
			li_sockaddr_to_string(config->sock_addr, wpool->wrk->tmp_str, TRUE)->str);

		--wpool->pending;
		--wpool->pool->pending;
		--wpool->pool->total;

		S_backend_pool_failed(wpool);
	}

	g_mutex_unlock(pool->lock);

	li_waitqueue_update(wq);
}

static void backend_con_watch_connect_cb(liEventBase *watcher, int events) {
	liEventIO *iowatcher = li_event_io_from(watcher);
	liBackendConnection_p *con = LI_CONTAINER_OF(iowatcher, liBackendConnection_p, public.watcher);
	liBackendPool_p *pool = con->pool;
	const liBackendConfig *config = pool->public.config;
	liBackendWorkerPool *wpool = &pool->worker_pools[con->worker->ndx];
	liServer *srv = wpool->wrk->srv;
	int fd = li_event_io_fd(iowatcher);
	struct sockaddr addr;
	socklen_t len;
	UNUSED(events);

	li_event_stop(iowatcher);
	li_waitqueue_remove(&wpool->connect_queue, &con->timeout_elem);

	g_mutex_lock(pool->lock);

	/* Check to see if we can determine our peer's address. */
	len = sizeof(addr);
	if (getpeername(fd, &addr, &len) == -1) {
		/* connect failed; find out why */
		int err;
		len = sizeof(err);
#ifdef SO_ERROR
		if (-1 == getsockopt(fd, SOL_SOCKET, SO_ERROR, (void*)&err, &len)) {
			err = errno;
		}
#else
		{
			char ch;
			errno = 0;
			read(fd, &ch, 1);
			err = errno;
		}
#endif
		ERROR(srv, "Couldn't connect to '%s': %s",
			li_sockaddr_to_string(config->sock_addr, wpool->wrk->tmp_str, TRUE)->str,
			g_strerror(err));

		close(fd);
		li_event_clear(iowatcher);
		g_slice_free(liBackendConnection_p, con);

		--wpool->pending;
		--wpool->pool->pending;
		--wpool->pool->total;

		S_backend_pool_failed(wpool);
	} else {
		/* connect succeeded */
		BACKEND_THREAD_CB(new, pool, wpool->wrk, con);

		if (pool->public.config->watch_for_close) {
			li_event_set_callback(&con->public.watcher, backend_con_watch_for_close_cb);
			li_event_io_set_events(&con->public.watcher, LI_EV_READ);
			li_event_start(&con->public.watcher);
		}
		li_waitqueue_push(&wpool->idle_queue, &con->timeout_elem);

		--wpool->pending;
		--wpool->pool->pending;
		--wpool->pool->total;
		S_backend_pool_worker_insert_con(pool, NULL, con);

		S_backend_pool_distribute(pool, wpool->wrk);
	}

	g_mutex_unlock(pool->lock);
}

static void S_backend_pool_worker_insert_pending(liBackendWorkerPool *wpool, int fd) {
	liBackendConnection_p *con = backend_connection_new(wpool);

	li_event_io_init(&wpool->wrk->loop, "backend connection", &con->public.watcher, backend_con_watch_connect_cb, fd, LI_EV_READ | LI_EV_WRITE);
	li_event_set_keep_loop_alive(&con->public.watcher, FALSE);
	li_event_start(&con->public.watcher);

	++wpool->pending;
	++wpool->pool->pending;
	++wpool->pool->total;
	li_waitqueue_push(&wpool->connect_queue, &con->timeout_elem);
}

static gboolean S_backend_connection_connect(liBackendWorkerPool *wpool) {
	const liBackendConfig *config = wpool->pool->public.config;
	liServer *srv = wpool->wrk->srv;
	int fd;

	do {
		fd = socket(config->sock_addr.addr_up.plain->sa_family, SOCK_STREAM, 0);
	} while (-1 == fd && errno == EINTR);
	if (-1 == fd) {
		if (errno == EMFILE) {
			li_server_out_of_fds(srv);
		}
		ERROR(srv, "Couldn't open socket: %s", g_strerror(errno));
		return FALSE;
	}
	li_fd_no_block(fd);

	if (-1 == connect(fd, config->sock_addr.addr_up.plain, config->sock_addr.len)) {
		switch (errno) {
		case EINPROGRESS:
		case EALREADY:
		case EINTR:
			S_backend_pool_worker_insert_pending(wpool, fd);
			return TRUE;
		default:
			ERROR(srv, "Couldn't connect to '%s': %s",
				li_sockaddr_to_string(config->sock_addr, wpool->wrk->tmp_str, TRUE)->str,
				g_strerror(errno));
			close(fd);
			return FALSE;
		}
	}

	S_backend_pool_worker_insert_connected(wpool, fd);
	return TRUE; /* successfully connected */
}

static void S_backend_pool_distribute(liBackendPool_p *pool, liWorker *wrk) {
	if (pool->public.config->max_connections <= 0) {
		int max_connections = (pool->public.config->max_connections < 0) ? -pool->public.config->max_connections : 128;
		liBackendWorkerPool *wpool = &pool->worker_pools[wrk->ndx];
		/* only local wpool */

		if (0 == wpool->wait_queue.length) return;

		while (wpool->wait_queue.length > 0 && wpool->idle > 0) {
			liBackendWait *bwait = LI_CONTAINER_OF(g_queue_pop_head_link(&wpool->wait_queue), liBackendWait, wait_queue_link);
			liBackendConnection_p *con = g_ptr_array_index(wpool->connections, wpool->active + wpool->reserved);

			bwait->con = con;
			con->wait = bwait;
			con->active = TRUE;
			S_backend_pool_worker_insert_con(pool, NULL, con);
			li_vrequest_joblist_append(bwait->vr);
		}

		if (MIN((unsigned int) max_connections, wpool->wait_queue.length) > wpool->pending) {
			guint need = MIN((unsigned int) max_connections, wpool->wait_queue.length) - wpool->pending;
			for (; need > 0; --need) {
				if (!S_backend_connection_connect(wpool)) {
					S_backend_pool_failed(wpool);
					return;
				}
			}

			/* recursive restart. should only recurse once... */
			S_backend_pool_distribute(pool, wrk);
		}
	} else {
		if (0 == pool->wait_queue.length) return;

		/* ERROR(wrk->srv, "pool wait queue: %i, pool idle: %i", pool->wait_queue.length, pool->idle); */

		if (pool->idle > 0) {
			/* distribute backends over all workers:
			 *  + first sort all "bwait"s we're going to assign to idle connections into the worker
			 *    specific queues
			 *  + 1. distribution round: for each worker assign connections from the worker itself
			 *  + 2. distribution round: assign connections from other workers
			 */
			liServer *srv = wrk->srv;
			guint worker_count = srv->worker_count;
			guint use = MIN(pool->idle, pool->wait_queue.length);

			{
				guint i;
				for (i = use; i > 0; --i) {
					liBackendWait *bwait = LI_CONTAINER_OF(g_queue_pop_head_link(&pool->wait_queue), liBackendWait, wait_queue_link);
					liBackendWorkerPool *wpool = &pool->worker_pools[bwait->vr->wrk->ndx];
					g_queue_push_tail_link(&wpool->wait_queue, &bwait->wait_queue_link);
				}
			}

			{
				guint i;
				for (i = 0; i < worker_count; ++i) {
					liBackendWorkerPool *wpool = &pool->worker_pools[i];

					/* ERROR(wrk->srv, "pool %i: queue: %i, idle: %i", i, wpool->wait_queue.length, wpool->idle); */

					while (wpool->wait_queue.length > 0 && wpool->idle > 0) {
						liBackendWait *bwait = LI_CONTAINER_OF(g_queue_pop_head_link(&wpool->wait_queue), liBackendWait, wait_queue_link);
						liBackendConnection_p *con = g_ptr_array_index(wpool->connections, wpool->active + wpool->reserved);

						bwait->con = con;
						con->wait = bwait;
						con->active = TRUE;
						S_backend_pool_worker_insert_con(pool, NULL, con);
						if (i == wrk->ndx) {
							li_vrequest_joblist_append(bwait->vr);
						} else {
							li_job_async(bwait->vr_ref);
						}
						--use;
					}
				}
			}

			if (use > 0) {
				guint src = 0;
				guint i;

				LI_FORCE_ASSERT(pool->idle >= use);
				for (i = 0; i < worker_count; ++i) {
					liBackendWorkerPool *wpool = &pool->worker_pools[i];

					while (wpool->wait_queue.length > 0) {
						liBackendWait *bwait = LI_CONTAINER_OF(g_queue_pop_head_link(&wpool->wait_queue), liBackendWait, wait_queue_link);
						liBackendConnection_p *con;
						liBackendWorkerPool *srcpool;

						while (0 == pool->worker_pools[src].idle) {
							++src;
							LI_FORCE_ASSERT(src < worker_count);
						}
						srcpool = &pool->worker_pools[src];
						con = g_ptr_array_index(srcpool->connections, srcpool->active + srcpool->reserved);

						bwait->con = con;
						con->wait = bwait;
						con->worker_next = g_array_index(srv->workers, liWorker*, i);
						S_backend_pool_worker_insert_con(pool, NULL, con);

						li_event_async_send(&srcpool->wakeup);
					}
				}
			}
		}

		if (pool->wait_queue.length > pool->pending) {
			guint need = MIN((unsigned int) pool->public.config->max_connections - pool->total, pool->wait_queue.length - pool->pending);
			if (need > 0) {
				liBackendWorkerPool *wpool = &pool->worker_pools[wrk->ndx];

				for (; need > 0; --need) {
					if (!S_backend_connection_connect(wpool)) {
						S_backend_pool_failed(wpool);
						return;
					}
				}

				/* recursive restart. should only recurse once... */
				S_backend_pool_distribute(pool, wrk);
			}
		}
	}
}

static void S_backend_wait_queue_unshift(GQueue *queue, GList *lnk) {
	if (0 == queue->length) {
		g_queue_push_head_link(queue, lnk);
	} else {
		liBackendWait *link_wait = LI_CONTAINER_OF(lnk, liBackendWait, wait_queue_link);
		GList *cursor = queue->head;
		liBackendWait *bwait = LI_CONTAINER_OF(cursor, liBackendWait, wait_queue_link);

		if (bwait->ts_started > link_wait->ts_started) {
			g_queue_push_head_link(queue, lnk);
			return;
		}

		do {
			cursor = cursor->next;
			if (NULL == cursor) {
				g_queue_push_tail_link(queue, lnk);
				return;
			}
			bwait = LI_CONTAINER_OF(cursor, liBackendWait, wait_queue_link);
		} while (bwait->ts_started < link_wait->ts_started);

		/* insert lnk before cursor; lnk will neither be the first nor the last element,
		 * so we don't have to update queue->head/tail
		 */
		lnk->next = cursor;
		lnk->prev = cursor->prev;
		cursor->prev->next = lnk;
		cursor->prev = lnk;
	}
}

static void backend_connection_close(liBackendPool_p *pool, liBackendConnection_p *con, gboolean have_lock) {
	liWorker *wrk = con->worker; /* only close attached connections */
	liBackendWorkerPool *wpool = &pool->worker_pools[wrk->ndx];
	int fd;

	if (!have_lock) g_mutex_lock(pool->lock);
	S_backend_pool_worker_remove_con(pool, con);
	if (NULL != con->wait) {
		con->wait->con = NULL;
		if (pool->public.config->max_connections <= 0) {
			S_backend_wait_queue_unshift(&wpool->wait_queue, &con->wait->wait_queue_link);
		} else {
			S_backend_wait_queue_unshift(&pool->wait_queue, &con->wait->wait_queue_link);
		}
		S_backend_pool_distribute(pool, con->wait->vr->wrk);
		con->wait = NULL;
	}
	if (!have_lock) g_mutex_unlock(pool->lock);

	/* don't need lock here */
	li_waitqueue_remove(&wpool->idle_queue, &con->timeout_elem);

	BACKEND_THREAD_CB(close, pool, wrk, con);

	fd = li_event_io_fd(&con->public.watcher);
	li_event_clear(&con->public.watcher);
	if (-1 != fd) li_worker_add_closing_socket(wrk, fd);

	g_slice_free(liBackendConnection_p, con);
}

static void backend_con_watch_for_close_cb(liEventBase *watcher, int events) {
	liEventIO *iowatcher = li_event_io_from(watcher);
	liBackendConnection_p *con = LI_CONTAINER_OF(iowatcher, liBackendConnection_p, public.watcher);
	liBackendPool_p *pool = con->pool;
	char c;
	int r;
	UNUSED(events);

	r = read(li_event_io_fd(iowatcher), &c, 1);
	if (-1 == r && (EAGAIN == errno || EWOULDBLOCK == errno || EINTR == errno)) return;

	/* TODO: log error when read data */

	backend_connection_close(pool, con, FALSE);
}

static void backend_pool_worker_run_reserved(liBackendWorkerPool *wpool) {
	liBackendPool_p *pool = wpool->pool;
	liWorker *wrk = wpool->wrk;

	g_mutex_lock(pool->lock);
	while (wpool->reserved > 0) {
		liBackendConnection_p *con = g_ptr_array_index(wpool->connections, wpool->active);
		if (NULL == con->worker) {
			/* attach */
			LI_FORCE_ASSERT(con->worker_next == wrk);
			con->worker = wrk;
			con->worker_next = NULL;

			li_event_attach(&wrk->loop, &con->public.watcher);
			li_waitqueue_push(&wpool->idle_queue, &con->timeout_elem);

			BACKEND_THREAD_CB(attach_thread, pool, wrk, con);

			if (-1 == li_event_io_fd(&con->public.watcher)) {
				backend_connection_close(pool, con, TRUE);
				continue;
			}

			if (NULL == con->wait) {
				S_backend_pool_worker_insert_con(pool, wrk, con);
				S_backend_pool_distribute(pool, wrk);
				continue;
			}
		}

		LI_FORCE_ASSERT(NULL != con->wait);

		if (NULL == con->worker_next) {
			LI_FORCE_ASSERT(con->wait->vr->wrk == wrk);
			if (!con->active) {
				con->active = TRUE;
				S_backend_pool_worker_insert_con(pool, wrk, con);
				li_vrequest_joblist_append(con->wait->vr);
			}
			continue;
		} else {
			/* detach and send to target worker */
			BACKEND_THREAD_CB(detach_thread, pool, wrk, con);

			if (-1 == li_event_io_fd(&con->public.watcher)) {
				backend_connection_close(pool, con, TRUE);
				continue;
			}
			li_event_detach(&con->public.watcher);

			li_waitqueue_remove(&wpool->idle_queue, &con->timeout_elem);
			S_backend_pool_worker_insert_con(pool, con->worker_next, con);
			con->worker = NULL;
			li_event_async_send(&pool->worker_pools[con->worker_next->ndx].wakeup);
		}
	}
	g_mutex_unlock(pool->lock);
}

static void backend_pool_worker_run(liEventBase *watcher, int events) {
	liEventAsync *async_watcher = li_event_async_from(watcher);
	liBackendWorkerPool *wpool = LI_CONTAINER_OF(async_watcher, liBackendWorkerPool, wakeup);
	UNUSED(events);

	backend_pool_worker_run_reserved(wpool);
}

static void backend_pool_worker_idle_timeout(liWaitQueue *wq, gpointer data) {
	liBackendWorkerPool *wpool = data;
	liBackendPool_p *pool = wpool->pool;
	liWaitQueueElem *elem;

	backend_pool_worker_run_reserved(wpool);

	while (NULL != (elem = li_waitqueue_pop(wq))) {
		liBackendConnection_p *con = LI_CONTAINER_OF(elem, liBackendConnection_p, timeout_elem);
		backend_connection_close(pool, con, FALSE);
	}

	li_waitqueue_update(wq);
}

static void S_backend_pool_update_wait_queue_timer(liBackendWorkerPool *wpool) {
	liBackendPool_p *pool = wpool->pool;

	if (pool->wait_queue.length > 0) {
		li_tstamp now = li_cur_ts(wpool->wrk);
		liBackendWait *bwait = LI_CONTAINER_OF(g_queue_peek_head_link(&pool->wait_queue), liBackendWait, wait_queue_link);
		li_tstamp repeat = bwait->ts_started + pool->public.config->wait_timeout - now;

		if (repeat < 0.05) repeat = 0.05;

		li_event_timer_once(&wpool->wait_queue_timer, repeat);
	} else {
		/* stop timer if queue empty */
		li_event_stop(&wpool->wait_queue_timer);
	}
}

static void backend_pool_wait_queue_timeout(liEventBase *watcher, int events) {
	liBackendWorkerPool *wpool = LI_CONTAINER_OF(li_event_timer_from(watcher), liBackendWorkerPool, wait_queue_timer);
	liBackendPool_p *pool = wpool->pool;
	li_tstamp due = li_cur_ts(wpool->wrk) - pool->public.config->wait_timeout;

	UNUSED(events);

	g_mutex_lock(pool->lock);

	while (pool->wait_queue.length > 0) {
		liBackendWait *bwait = LI_CONTAINER_OF(g_queue_peek_head_link(&pool->wait_queue), liBackendWait, wait_queue_link);

		if (bwait->ts_started <= due) {
			g_queue_pop_head_link(&pool->wait_queue);
			bwait->failed = TRUE;
			li_job_async(bwait->vr_ref);
		} else {
			break;
		}
	}

	S_backend_pool_update_wait_queue_timer(wpool);

	g_mutex_unlock(pool->lock);
}

static gpointer backend_pool_worker_init(liWorker *wrk, gpointer fdata) {
	liBackendPool_p *pool = fdata;
	liBackendWorkerPool *wpool = &pool->worker_pools[wrk->ndx];
	guint idle_timeout = pool->public.config->idle_timeout;

	if (wpool->initialized) return NULL;

	li_event_async_init(&wrk->loop, "backend manager", &wpool->wakeup, backend_pool_worker_run);
	if (idle_timeout < 1) idle_timeout = 5;
	li_waitqueue_init(&wpool->idle_queue, &wrk->loop, "backend idle queue", backend_pool_worker_idle_timeout, idle_timeout, wpool);
	li_waitqueue_init(&wpool->connect_queue, &wrk->loop, "backend connect queue", backend_pool_worker_connect_timeout, pool->public.config->connect_timeout, wpool);

	li_event_timer_init(&wrk->loop, "backend wait timeout", &wpool->wait_queue_timer, backend_pool_wait_queue_timeout);
	li_event_set_keep_loop_alive(&wpool->wait_queue_timer, FALSE);

	wpool->initialized = TRUE;
	return NULL;
}

static void backend_pool_worker_init_done(liWorker *wrk, gpointer cbdata, gpointer fdata, GPtrArray *result, gboolean complete) {
	liBackendPool_p *pool = fdata;
	UNUSED(wrk);
	UNUSED(cbdata);
	UNUSED(result);
	UNUSED(complete);

	pool->initialized = TRUE;
}

static void S_backend_pool_init(liWorker *wrk, liBackendPool_p *pool) {
	LI_FORCE_ASSERT(!pool->shutdown);

	if (pool->initialized) return;

	if (pool->worker_pools == NULL) {
		guint i, l = wrk->srv->worker_count;
		pool->worker_pools = g_slice_alloc0(sizeof(liBackendWorkerPool) * l);

		for (i = 0; i < l; ++i) {
			liBackendWorkerPool *wpool = &pool->worker_pools[i];
			wpool->wrk = g_array_index(wrk->srv->workers, liWorker*, i);
			wpool->pool = pool;
			wpool->connections = g_ptr_array_new();
		}

		li_collect_start(wrk, backend_pool_worker_init, pool, backend_pool_worker_init_done, NULL);
	}

	backend_pool_worker_init(wrk, pool);
}

static gpointer backend_pool_worker_shutdown(liWorker *wrk, gpointer fdata) {
	liBackendPool_p *pool = fdata;
	liBackendWorkerPool *wpool = &pool->worker_pools[wrk->ndx];
	liWaitQueueElem *elem;

	backend_pool_worker_run_reserved(wpool);

	li_event_clear(&wpool->wakeup);
	li_event_clear(&wpool->wait_queue_timer);

	g_mutex_lock(pool->lock);

	while (NULL != (elem = li_waitqueue_pop_force(&wpool->idle_queue))) {
		liBackendConnection_p *con = LI_CONTAINER_OF(elem, liBackendConnection_p, timeout_elem);
		backend_connection_close(pool, con, TRUE);
	}
	li_waitqueue_stop(&wpool->idle_queue);

	while (NULL != (elem = li_waitqueue_pop_force(&wpool->connect_queue))) {
		liBackendConnection_p *con = LI_CONTAINER_OF(elem, liBackendConnection_p, timeout_elem);
		int fd = li_event_io_fd(&con->public.watcher);
		li_event_clear(&con->public.watcher);
		close(fd);
		g_slice_free(liBackendConnection_p, con);

		--wpool->pending;
		--wpool->pool->pending;
		--wpool->pool->total;
	}
	li_waitqueue_stop(&wpool->connect_queue);

	LI_FORCE_ASSERT(0 == wpool->active);
	LI_FORCE_ASSERT(0 == wpool->reserved);
	LI_FORCE_ASSERT(0 == wpool->idle);
	LI_FORCE_ASSERT(0 == wpool->pending);

	g_ptr_array_free(wpool->connections, TRUE);

	g_mutex_unlock(pool->lock);

	return NULL;
}

static void backend_pool_worker_shutdown_done(liWorker *wrk, gpointer cbdata, gpointer fdata, GPtrArray *result, gboolean complete) {
	liBackendPool_p *pool = fdata;
	UNUSED(cbdata);
	UNUSED(result);
	UNUSED(complete);

	pool->public.config->callbacks->free_cb(&pool->public);

	if (pool->worker_pools != NULL) {
		g_slice_free1(sizeof(liBackendPool_p) * wrk->srv->worker_count, pool->worker_pools);
	}

	g_mutex_free(pool->lock);
	g_slice_free(liBackendPool_p, pool);
}


liBackendPool* li_backend_pool_new(const liBackendConfig *config) {
	liBackendPool_p *pool = g_slice_new0(liBackendPool_p);
	pool->public.config = config;
	pool->lock = g_mutex_new();

	pool->ts_disabled_till = 0;

	return &pool->public;
}

void li_backend_pool_free(liBackendPool *bpool) {
	liBackendPool_p *pool = LI_CONTAINER_OF(bpool, liBackendPool_p, public);

	g_mutex_lock(pool->lock);

	LI_FORCE_ASSERT(0 == pool->active);
	LI_FORCE_ASSERT(!pool->shutdown);

	pool->shutdown = TRUE;

	g_mutex_unlock(pool->lock);

	if (pool->worker_pools == NULL) {
		backend_pool_worker_shutdown_done(NULL, NULL, pool, NULL, TRUE);
	} else {
		liServer *srv = pool->worker_pools[0].wrk->srv;

		/*ERROR(srv, "li_backend_pool_free: %p, reserved: %i, idle: %i, pending: %i", (void*) pool,
			pool->reserved, pool->idle, pool->pending); */
		li_collect_start_global(srv, backend_pool_worker_shutdown, pool, backend_pool_worker_shutdown_done, NULL);
	}
}

liBackendResult li_backend_get(liVRequest *vr, liBackendPool *bpool, liBackendConnection **pbcon, liBackendWait **pbwait) {
	liBackendPool_p *pool = LI_CONTAINER_OF(bpool, liBackendPool_p, public);
	liBackendWorkerPool *wpool;
	liBackendResult result = LI_BACKEND_TIMEOUT;
	liBackendWait *bwait = NULL;

	LI_FORCE_ASSERT(pbcon);
	LI_FORCE_ASSERT(pbwait);

	g_mutex_lock(pool->lock);
	S_backend_pool_init(vr->wrk, pool);

	wpool = &pool->worker_pools[vr->wrk->ndx];

	if (*pbwait) {
		bwait = *pbwait;
		LI_FORCE_ASSERT(vr == bwait->vr);
	} else if (pool->ts_disabled_till > li_cur_ts(vr->wrk)) {
		goto out;
	} else {
		if (wpool->idle > 0) {
			/* shortcut without backend_pool_distribute */
			liBackendConnection_p *con = g_ptr_array_index(wpool->connections, wpool->active + wpool->reserved);
			con->active = TRUE;
			S_backend_pool_worker_insert_con(pool, NULL, con);
			*pbcon = &con->public;
			result = LI_BACKEND_SUCCESS;
			li_event_set_keep_loop_alive(&con->public.watcher, TRUE);
			if (pool->public.config->watch_for_close) {
				li_event_stop(&con->public.watcher);
				li_event_set_callback(&con->public.watcher, NULL);
			}
			li_waitqueue_remove(&wpool->idle_queue, &con->timeout_elem);
			goto out;
		}

		bwait = g_slice_new0(liBackendWait);
		bwait->vr = vr;
		bwait->vr_ref = li_vrequest_get_ref(vr);
		bwait->ts_started = li_cur_ts(vr->wrk);
		*pbwait = bwait;

		if (pool->public.config->max_connections <= 0) {
			g_queue_push_tail_link(&wpool->wait_queue, &bwait->wait_queue_link);
		} else {
			g_queue_push_tail_link(&pool->wait_queue, &bwait->wait_queue_link);
			S_backend_pool_update_wait_queue_timer(wpool);
		}
		S_backend_pool_distribute(pool, vr->wrk);
	}

	LI_FORCE_ASSERT(bwait);

	if (bwait->failed) {
		bwait->vr = NULL;
		li_job_ref_release(bwait->vr_ref);
		bwait->vr_ref = NULL;
		bwait->failed = FALSE;
		g_slice_free(liBackendWait, bwait);
		*pbwait = NULL;
		result = LI_BACKEND_TIMEOUT;
		goto out;
	}

	if (bwait->con && bwait->con->worker == vr->wrk) {
		liBackendConnection_p *con = bwait->con;
		bwait->con = NULL;
		bwait->vr = NULL;
		li_job_ref_release(bwait->vr_ref);
		bwait->vr_ref = NULL;
		g_slice_free(liBackendWait, bwait);
		*pbwait = NULL;
		*pbcon = &con->public;

		con->wait = NULL;
		con->active = TRUE;
		S_backend_pool_worker_insert_con(pool, NULL, con);

		result = LI_BACKEND_SUCCESS;
		li_event_set_keep_loop_alive(&con->public.watcher, TRUE);
		if (pool->public.config->watch_for_close) {
			li_event_stop(&con->public.watcher);
			li_event_set_callback(&con->public.watcher, NULL);
		}
		li_waitqueue_remove(&wpool->idle_queue, &con->timeout_elem);
		goto out;
	}

	result = LI_BACKEND_WAIT;

out:
	g_mutex_unlock(pool->lock);

	return result;
}

void li_backend_wait_stop(liVRequest *vr, liBackendPool *bpool, liBackendWait **pbwait) {
	liBackendPool_p *pool = LI_CONTAINER_OF(bpool, liBackendPool_p, public);
	liBackendWait *bwait;

	LI_FORCE_ASSERT(pbwait);
	bwait = *pbwait;

	if (!bwait) return;

	*pbwait = NULL;

	LI_FORCE_ASSERT(vr == bwait->vr);

	if (bwait->failed) {
		bwait->vr = NULL;
		li_job_ref_release(bwait->vr_ref);
		bwait->vr_ref = NULL;
		bwait->failed = FALSE;
		g_slice_free(liBackendWait, bwait);
		return;
	}

	g_mutex_lock(pool->lock);

	if (!bwait->failed) {
		if (bwait->con) {
			bwait->con->wait = NULL;
			bwait->con->active = FALSE;
			S_backend_pool_worker_insert_con(pool, NULL, bwait->con);
			bwait->con = NULL;
		} else if (pool->public.config->max_connections <= 0) {
			liBackendWorkerPool *wpool = &pool->worker_pools[bwait->vr->wrk->ndx];
			g_queue_unlink(&wpool->wait_queue, &bwait->wait_queue_link);
		} else {
			g_queue_unlink(&pool->wait_queue, &bwait->wait_queue_link);
		}
	}

	bwait->vr = NULL;
	li_job_ref_release(bwait->vr_ref);
	bwait->vr_ref = NULL;
	g_slice_free(liBackendWait, bwait);

	g_mutex_unlock(pool->lock);
}

void li_backend_put(liWorker *wrk, liBackendPool *bpool, liBackendConnection *bcon, gboolean closecon) {
	liBackendPool_p *pool = LI_CONTAINER_OF(bpool, liBackendPool_p, public);
	liBackendConnection_p *con = LI_CONTAINER_OF(bcon, liBackendConnection_p, public);
	liBackendWorkerPool *wpool = &pool->worker_pools[wrk->ndx];

	++con->requests;
	con->active = FALSE;

	if (-1 == li_event_io_fd(&con->public.watcher) || closecon
		|| (pool->public.config->max_requests > 0 && con->requests >= pool->public.config->max_requests)
		|| (0 == pool->public.config->idle_timeout)) {
		backend_connection_close(pool, con, FALSE);
	} else {
		g_mutex_lock(pool->lock);

		pool->ts_disabled_till = 0;

		li_event_set_keep_loop_alive(&con->public.watcher, FALSE);
		if (pool->public.config->watch_for_close) {
			li_event_set_callback(&con->public.watcher, backend_con_watch_for_close_cb);
			li_event_io_set_events(&con->public.watcher, LI_EV_READ);
			li_event_start(&con->public.watcher);
		}
		li_waitqueue_push(&wpool->idle_queue, &con->timeout_elem);

		S_backend_pool_worker_insert_con(pool, NULL, con);

		S_backend_pool_distribute(pool, wrk);

		g_mutex_unlock(pool->lock);
	}
}

void li_backend_connection_closed(liBackendPool *bpool, liBackendConnection *bcon) {
	liBackendPool_p *pool = LI_CONTAINER_OF(bpool, liBackendPool_p, public);
	liBackendConnection_p *con = LI_CONTAINER_OF(bcon, liBackendConnection_p, public);

	backend_connection_close(pool, con, FALSE);
}

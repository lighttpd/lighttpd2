
#include <lighttpd/base.h>

#include <lighttpd/plugin_core.h>

static liConnection* worker_con_get(liWorker *wrk);

/* closing sockets - wait for proper shutdown */

void li_worker_add_closing_socket(liWorker *wrk, int fd) {
	liServerState state = g_atomic_int_get(&wrk->srv->state);

	if (-1 == fd) return;

	if (LI_SERVER_RUNNING != state && LI_SERVER_WARMUP != state) {
		shutdown(fd, SHUT_WR);
		close(fd);
		return;
	}

	li_event_add_closing_socket(&wrk->loop, fd);
}

/* Keep alive */

void li_worker_check_keepalive(liWorker *wrk) {
	li_tstamp now = li_cur_ts(wrk);

	if (0 == wrk->keep_alive_queue.length) {
		li_event_stop(&wrk->keep_alive_timer);
	} else {
		li_tstamp timeout = ((liConnection*)g_queue_peek_head(&wrk->keep_alive_queue))->keep_alive_data.timeout - now + 1;
		li_event_timer_once(&wrk->keep_alive_timer, timeout);
	}
}

static void worker_keepalive_cb(liEventBase *watcher, int events) {
	liWorker *wrk = LI_CONTAINER_OF(li_event_timer_from(watcher), liWorker, keep_alive_timer);
	ev_tstamp now = li_cur_ts(wrk);
	GQueue *q = &wrk->keep_alive_queue;
	GList *l;
	liConnection *con;

	UNUSED(events);

	while ( NULL != (l = g_queue_peek_head_link(q)) &&
	        (con = (liConnection*) l->data)->keep_alive_data.timeout <= now ) {
		ev_tstamp remaining = con->keep_alive_data.max_idle - wrk->srv->keep_alive_queue_timeout - (now - con->keep_alive_data.timeout);
		if (remaining > 0) {
			g_queue_delete_link(q, l);
			con->keep_alive_data.link = NULL;
			li_event_timer_once(&con->keep_alive_data.watcher, remaining);
		} else {
			/* close it */
			li_connection_reset(con);
		}
	}

	if (NULL == l) {
		li_event_stop(&wrk->keep_alive_timer);
	} else {
		li_event_timer_once(&wrk->keep_alive_timer, con->keep_alive_data.timeout - now + 1);
	}
}

/* check for timeouted connections */
static void worker_io_timeout_cb(liWaitQueue *wq, gpointer data) {
	liWorker *wrk = data;
	liConnection *con;
	liWaitQueueElem *wqe;
	ev_tstamp now = li_cur_ts(wrk);

	while ((wqe = li_waitqueue_pop(wq)) != NULL) {
		liVRequest *vr;
		/* connection has timed out */
		con = wqe->data;
		vr = con->mainvr;
		if (CORE_OPTION(LI_CORE_OPTION_DEBUG_REQUEST_HANDLING).boolean) {
			VR_DEBUG(vr, "connection io-timeout from %s after %.2f seconds", con->info.remote_addr_str->str, now - wqe->ts);
		}
		li_plugins_handle_close(con);
		li_worker_con_put(con);
	}

	li_waitqueue_update(wq);
}

/* cache timestamp */
GString *li_worker_current_timestamp(liWorker *wrk, liTimeFunc timefunc, guint format_ndx) {
	gsize len;
	struct tm tm;
	liWorkerTS *wts;
	time_t now = (time_t)li_cur_ts(wrk);
	GArray *a;

	if (timefunc == LI_GMTIME) {
		a = wrk->timestamps_gmt;
	} else {
		a = wrk->timestamps_local;
	}

	wts = &g_array_index(a, liWorkerTS, format_ndx);

	/* cache hit */
	if (now == wts->last_generated)
		return wts->str;

	if (timefunc == LI_GMTIME) {
		if (!gmtime_r(&now, &tm))
			return NULL;
	} else if (!localtime_r(&now, &tm))
		return NULL;

	g_string_set_size(wts->str, 255);

	len = strftime(wts->str->str, wts->str->allocated_len, g_array_index(wrk->srv->ts_formats, GString*, format_ndx)->str, &tm);

	if (len == 0)
		return NULL;

	g_string_set_size(wts->str, len);
	wts->last_generated = now;

	return wts->str;
}

static void li_worker_prepare_cb(liEventBase *watcher, int events) {
	liWorker *wrk = LI_CONTAINER_OF(li_event_prepare_from(watcher), liWorker, loop_prepare);
	liServer *srv = wrk->srv;
	UNUSED(events);

	/* are there pending log entries? */
	if (g_queue_get_length(&wrk->logs.log_queue)) {
		/* take log entries from local queue, insert into global queue and notify log thread */
		g_static_mutex_lock(&srv->logs.write_queue_mutex);

		li_g_queue_merge(&srv->logs.write_queue, &wrk->logs.log_queue);

		g_static_mutex_unlock(&srv->logs.write_queue_mutex);
		li_event_async_send(&srv->logs.watcher);
	}
}

/* stop worker watcher */
static void li_worker_stop_cb(liEventBase *watcher, int events) {
	liWorker *wrk = LI_CONTAINER_OF(li_event_async_from(watcher), liWorker, worker_stop_watcher);
	UNUSED(events);

	li_worker_stop(wrk, wrk);
}

/* stopping worker watcher */
static void li_worker_stopping_cb(liEventBase *watcher, int events) {
	liWorker *wrk = LI_CONTAINER_OF(li_event_async_from(watcher), liWorker, worker_stopping_watcher);
	UNUSED(events);

	li_worker_stopping(wrk, wrk);
}

/* suspend worker watcher */
static void li_worker_suspend_cb(liEventBase *watcher, int events) {
	liWorker *wrk = LI_CONTAINER_OF(li_event_async_from(watcher), liWorker, worker_suspend_watcher);
	UNUSED(events);

	li_worker_suspend(wrk, wrk);
}

/* exit worker watcher */
static void li_worker_exit_cb(liEventBase *watcher, int events) {
	liWorker *wrk = LI_CONTAINER_OF(li_event_async_from(watcher), liWorker, worker_exit_watcher);
	UNUSED(events);

	li_worker_exit(wrk, wrk);
}

typedef struct li_worker_new_con_data li_worker_new_con_data;
struct li_worker_new_con_data {
	liSocketAddress remote_addr;
	int s;
	liServerSocket *srv_sock;
};

/* new con watcher */
void li_worker_new_con(liWorker *ctx, liWorker *wrk, liSocketAddress remote_addr, int s, liServerSocket *srv_sock) {
	if (ctx == wrk) {
		liConnection *con = worker_con_get(wrk);

		li_connection_start(con, remote_addr, s, srv_sock);
	} else {
		li_worker_new_con_data *d = g_slice_new(li_worker_new_con_data);
		d->remote_addr = remote_addr;
		d->s = s;
		d->srv_sock = srv_sock;
		g_async_queue_push(wrk->new_con_queue, d);
		li_event_async_send(&wrk->new_con_watcher);
	}
}

static void li_worker_new_con_cb(liEventBase *watcher, int events) {
	liWorker *wrk = LI_CONTAINER_OF(li_event_async_from(watcher), liWorker, new_con_watcher);
	li_worker_new_con_data *d;
	UNUSED(events);

	while (NULL != (d = g_async_queue_try_pop(wrk->new_con_queue))) {
		li_worker_new_con(wrk, wrk, d->remote_addr, d->s, d->srv_sock);
		g_slice_free(li_worker_new_con_data, d);
	}
}

/* stats watcher */
static void worker_stats_watcher_cb(liEventBase *watcher, int events) {
	liWorker *wrk = LI_CONTAINER_OF(li_event_timer_from(watcher), liWorker, stats_watcher);
	ev_tstamp now = li_cur_ts(wrk);
	UNUSED(events);

	if (wrk->stats.last_update && now != wrk->stats.last_update) {
		wrk->stats.requests_per_sec =
			(wrk->stats.requests - wrk->stats.last_requests) / (now - wrk->stats.last_update);
#if 0
			if (wrk->stats.requests_per_sec > 0)
			DEBUG(wrk->srv, "worker %u: %.2f requests per second", wrk->ndx, wrk->stats.requests_per_sec);
#endif
	}

	/* 5s averages and peak values */
	if ((now - wrk->stats.last_avg) > 5) {
		/* bytes in */
		wrk->stats.bytes_in_5s_diff = wrk->stats.bytes_in - wrk->stats.bytes_in_5s;
		wrk->stats.bytes_in_5s = wrk->stats.bytes_in;
		wrk->stats.peak.bytes_in = MAX(wrk->stats.peak.bytes_in, wrk->stats.bytes_in_5s_diff / 5);

		/* bytes out */
		wrk->stats.bytes_out_5s_diff = wrk->stats.bytes_out - wrk->stats.bytes_out_5s;
		wrk->stats.bytes_out_5s = wrk->stats.bytes_out;
		wrk->stats.peak.bytes_out = MAX(wrk->stats.peak.bytes_out, wrk->stats.bytes_out_5s_diff / 5);

		/* requests */
		wrk->stats.requests_5s_diff = wrk->stats.requests - wrk->stats.requests_5s;
		wrk->stats.requests_5s = wrk->stats.requests;
		wrk->stats.peak.requests = MAX(wrk->stats.peak.requests, wrk->stats.requests_5s_diff / 5);

		/* active connections */
		wrk->stats.active_cons_5s = wrk->connections_active;
		wrk->stats.peak.active_cons = MAX(wrk->stats.peak.active_cons, wrk->connections_active);

		wrk->stats.last_avg = now;
	}

	wrk->stats.active_cons_cum += wrk->connections_active;

	wrk->stats.last_requests = wrk->stats.requests;
	wrk->stats.last_update = now;

	/* and run again next second */
	li_event_timer_once(&wrk->stats_watcher, 1);
}

/* init */

liWorker* li_worker_new(liServer *srv, struct ev_loop *loop) {
	liWorker *wrk = g_slice_new0(liWorker);
	wrk->srv = srv;
	li_event_loop_init(&wrk->loop, loop);

	li_lua_init(&wrk->LL, srv, wrk);

	g_queue_init(&wrk->keep_alive_queue);
	li_event_timer_init(&wrk->loop, &wrk->keep_alive_timer, worker_keepalive_cb);

	wrk->connections_active = 0;
	wrk->connections = g_array_new(FALSE, TRUE, sizeof(liConnection*));

	wrk->tmp_str = g_string_sized_new(255);

	wrk->timestamps_gmt = g_array_sized_new(FALSE, TRUE, sizeof(liWorkerTS), srv->ts_formats->len);
	g_array_set_size(wrk->timestamps_gmt, srv->ts_formats->len);
	{
		guint i;
		for (i = 0; i < srv->ts_formats->len; i++)
			g_array_index(wrk->timestamps_gmt, liWorkerTS, i).str = g_string_sized_new(255);
	}
	wrk->timestamps_local = g_array_sized_new(FALSE, TRUE, sizeof(liWorkerTS), srv->ts_formats->len);
	g_array_set_size(wrk->timestamps_local, srv->ts_formats->len);
	{
		guint i;
		for (i = 0; i < srv->ts_formats->len; i++)
			g_array_index(wrk->timestamps_local, liWorkerTS, i).str = g_string_sized_new(255);
	}

	li_event_prepare_init(&wrk->loop, &wrk->loop_prepare, li_worker_prepare_cb);
	li_event_async_init(&wrk->loop, &wrk->worker_stop_watcher, li_worker_stop_cb);
	li_event_async_init(&wrk->loop, &wrk->worker_stopping_watcher, li_worker_stopping_cb);
	li_event_async_init(&wrk->loop, &wrk->worker_exit_watcher, li_worker_exit_cb);
	li_event_async_init(&wrk->loop, &wrk->worker_suspend_watcher, li_worker_suspend_cb);

	li_event_async_init(&wrk->loop, &wrk->new_con_watcher, li_worker_new_con_cb);
	wrk->new_con_queue = g_async_queue_new();

	li_event_timer_init(&wrk->loop, &wrk->stats_watcher, worker_stats_watcher_cb);
	li_event_set_keep_loop_alive(&wrk->stats_watcher, FALSE);
	li_event_timer_once(&wrk->stats_watcher, 1);

	li_event_async_init(&wrk->loop, &wrk->collect_watcher, li_collect_watcher_cb);
	wrk->collect_queue = g_async_queue_new();

	/* io timeout timer */
	li_waitqueue_init(&wrk->io_timeout_queue, &wrk->loop, worker_io_timeout_cb, srv->io_timeout, wrk);

	/* throttling */
	li_waitqueue_init(&wrk->throttle_queue, &wrk->loop, li_throttle_cb, ((gdouble)THROTTLE_GRANULARITY) / 1000, wrk);

	wrk->tasklets = li_tasklet_pool_new(&wrk->loop, srv->tasklet_pool_threads);

	wrk->network_read_buf = NULL;

	return wrk;
}

struct ev_loop* li_worker_free(liWorker *wrk) {
	struct ev_loop* evloop;

	if (!wrk) return NULL;

	li_job_queue_clear(&wrk->loop.jobqueue);

	{ /* close connections */
		guint i;
		if (wrk->connections_active > 0) {
			ERROR(wrk->srv, "Server shutdown with unclosed connections: %u", wrk->connections_active);
			for (i = wrk->connections_active; i-- > 0;) {
				liConnection *con = g_array_index(wrk->connections, liConnection*, i);
				li_connection_error(con);
			}
		}
		for (i = 0; i < wrk->connections->len; i++) {
			liConnection *con = g_array_index(wrk->connections, liConnection*, i);
			li_connection_reset(con);
			li_connection_free(con);
		}
		g_array_free(wrk->connections, TRUE);
	}

	{ /* free timestamps */
		guint i;
		for (i = 0; i < wrk->timestamps_gmt->len; i++) {
			g_string_free(g_array_index(wrk->timestamps_gmt, liWorkerTS, i).str, TRUE);
			g_string_free(g_array_index(wrk->timestamps_local, liWorkerTS, i).str, TRUE);
		}
		g_array_free(wrk->timestamps_gmt, TRUE);
		g_array_free(wrk->timestamps_local, TRUE);
	}

	li_waitqueue_stop(&wrk->io_timeout_queue);
	li_waitqueue_stop(&wrk->throttle_queue);

	li_event_clear(&wrk->worker_stop_watcher);
	li_event_clear(&wrk->worker_stopping_watcher);
	li_event_clear(&wrk->worker_suspend_watcher);
	li_event_clear(&wrk->worker_exit_watcher);

	li_event_clear(&wrk->new_con_watcher);
	g_async_queue_unref(wrk->new_con_queue);
	wrk->new_con_queue = NULL;

	li_event_clear(&wrk->stats_watcher);

	li_collect_watcher_cb(&wrk->collect_watcher.base, 0);
	li_event_clear(&wrk->collect_watcher);
	g_async_queue_unref(wrk->collect_queue);
	wrk->collect_queue = NULL;

	li_event_clear(&wrk->loop_prepare);

	g_string_free(wrk->tmp_str, TRUE);

	li_stat_cache_free(wrk->stat_cache);

	li_tasklet_pool_free(wrk->tasklets);

	li_lua_clear(&wrk->LL);

	li_buffer_release(wrk->network_read_buf);

	evloop = li_event_loop_clear(&wrk->loop);

	g_slice_free(liWorker, wrk);

	return evloop;
}

void li_worker_run(liWorker *wrk) {
	/* update and start io timeout queue since first worker is allocated before srv->io_timeout is set */
	li_waitqueue_set_delay(&wrk->io_timeout_queue, wrk->srv->io_timeout);
	li_waitqueue_update(&wrk->io_timeout_queue);

	/* initialize timestamp caches for new ones that have been added by modules */
	if (wrk->srv->ts_formats->len > wrk->timestamps_gmt->len) {
		guint i = wrk->timestamps_gmt->len;
		g_array_set_size(wrk->timestamps_gmt, wrk->srv->ts_formats->len);
		g_array_set_size(wrk->timestamps_local, wrk->srv->ts_formats->len);
		for (; i < wrk->srv->ts_formats->len; i++) {
			g_array_index(wrk->timestamps_gmt, liWorkerTS, i).last_generated = 0;
			g_array_index(wrk->timestamps_gmt, liWorkerTS, i).str = g_string_sized_new(255);
			g_array_index(wrk->timestamps_local, liWorkerTS, i).last_generated = 0;
			g_array_index(wrk->timestamps_local, liWorkerTS, i).str = g_string_sized_new(255);
		}
	}

	/* setup stat cache if necessary */
	if (wrk->srv->stat_cache_ttl && !wrk->stat_cache)
		wrk->stat_cache = li_stat_cache_new(wrk, wrk->srv->stat_cache_ttl);

	li_event_loop_run(&wrk->loop);
}

void li_worker_stop(liWorker *context, liWorker *wrk) {
	if (context == wrk) {
		guint i;

		li_plugins_worker_stop(wrk);

		li_event_stop(&wrk->worker_stop_watcher);
		li_event_stop(&wrk->worker_stopping_watcher);
		li_event_stop(&wrk->worker_suspend_watcher);

		li_event_stop(&wrk->new_con_watcher);

		if (wrk->stat_cache)
			li_waitqueue_stop(&wrk->stat_cache->delete_queue);
		li_worker_new_con_cb(&wrk->new_con_watcher.base, 0); /* handle remaining new connections */

		/* close keep alive connections */
		for (i = wrk->connections_active; i-- > 0;) {
			liConnection *con = g_array_index(wrk->connections, liConnection*, i);
			if (con->state == LI_CON_STATE_KEEP_ALIVE) {
				li_worker_con_put(con);
			}
		}

		li_worker_check_keepalive(wrk);

		li_event_loop_end(&wrk->loop);
	} else {
		li_event_async_send(&wrk->worker_stop_watcher);
	}
}

void li_worker_stopping(liWorker *context, liWorker *wrk) {
	liServer *srv = context->srv;
	guint i;

	if (context == srv->main_worker) {
		li_server_state_wait(srv, &wrk->wait_for_stop_connections);
	}

	if (context == wrk) {
		/* li_plugins_worker_stopping(wrk); ??? */

		/* close keep alive connections */
		for (i = wrk->connections_active; i-- > 0;) {
			liConnection *con = g_array_index(wrk->connections, liConnection*, i);
			if (con->state == LI_CON_STATE_KEEP_ALIVE) {
				li_worker_con_put(con);
			}
		}

		li_worker_check_keepalive(wrk);

		li_worker_new_con_cb(&wrk->new_con_watcher.base, 0); /* handle remaining new connections */
		if (0 == g_atomic_int_get(&wrk->connection_load) && wrk->wait_for_stop_connections.active) {
			li_server_state_ready(srv, &wrk->wait_for_stop_connections);
		}

		li_event_loop_force_close_sockets(&wrk->loop);
	} else {
		li_event_async_send(&wrk->worker_stopping_watcher);
	}
}

void li_worker_suspend(liWorker *context, liWorker *wrk) {
	if (context == wrk) {
		guint i;

		/* close keep alive connections */
		for (i = wrk->connections_active; i-- > 0;) {
			liConnection *con = g_array_index(wrk->connections, liConnection*, i);
			if (con->state == LI_CON_STATE_KEEP_ALIVE) {
				li_worker_con_put(con);
			}
		}

		li_worker_check_keepalive(wrk);

		li_event_loop_force_close_sockets(&wrk->loop);

#if 0
		ERROR(wrk->srv, "%i connections still active", (int) wrk->connections_active);
		for (i = wrk->connections_active; i-- > 0;) {
			liConnection *con = g_array_index(wrk->connections, liConnection*, i);
			ERROR(wrk->srv, "%i state: %s, %i", i, li_connection_state_str(con->state), con->mainvr->state);
		}
#endif
	} else {
		li_event_async_send(&wrk->worker_suspend_watcher);
	}
}

void li_worker_exit(liWorker *context, liWorker *wrk) {
	if (context == wrk) {
		li_event_loop_exit(&wrk->loop);
	} else {
		li_event_async_send(&wrk->worker_exit_watcher);
	}
}


static liConnection* worker_con_get(liWorker *wrk) {
	liConnection *con;

	if (wrk->connections_active >= wrk->connections->len) {
		con = li_connection_new(wrk);
		con->idx = wrk->connections_active;
		g_array_append_val(wrk->connections, con);
	} else {
		con = g_array_index(wrk->connections, liConnection*, wrk->connections_active);
		con->idx = wrk->connections_active;
	}

	g_atomic_int_inc((gint*) &wrk->connections_active);

	if (wrk->connections_active > wrk->connections_active_max_5min)
		wrk->connections_active_max_5min = wrk->connections_active;

	return con;
}

void li_worker_con_put(liConnection *con) {
	liWorker *wrk = con->wrk;
	ev_tstamp now = li_cur_ts(wrk);

	if (-1 == (gint) con->idx)
		/* already inactive connection */
		return;

	g_atomic_int_add((gint*) &wrk->srv->connection_load, -1);
	g_atomic_int_add((gint*) &wrk->connection_load, -1);
	g_atomic_int_add((gint*) &wrk->connections_active, -1);

	if (con->idx != wrk->connections_active) {
		/* Swap [con->idx] and [wrk->connections_active] */
		liConnection *tmp;
		assert(con->idx < wrk->connections_active); /* con must be an active connection */
		tmp = g_array_index(wrk->connections, liConnection*, wrk->connections_active);
		tmp->idx = con->idx;
		con->idx = -1;
		g_array_index(wrk->connections, liConnection*, wrk->connections_active) = con;
		g_array_index(wrk->connections, liConnection*, tmp->idx) = tmp;
	}

	/* free unused connections. we keep max(connections_active) for the past 5min allocated */
	if ((now - wrk->connections_gc_ts) > 300.0) {
		guint i;

		for (i = wrk->connections->len; i > wrk->connections_active_max_5min; i--) {
			li_connection_free(g_array_index(wrk->connections, liConnection*, i-1));
			g_array_index(wrk->connections, liConnection*, i-1) = NULL;
		}

		wrk->connections->len = wrk->connections_active_max_5min;
		wrk->connections_active_max_5min = wrk->connections_active;
		wrk->connections_gc_ts = now;
	}


	if (wrk->wait_for_stop_connections.active && 0 == g_atomic_int_get((gint*) &wrk->connection_load)) {
		li_server_state_ready(wrk->srv, &wrk->wait_for_stop_connections);
	}
}

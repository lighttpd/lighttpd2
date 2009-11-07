
#include <lighttpd/base.h>

#include <lighttpd/plugin_core.h>

#ifdef HAVE_LUA_H
# include <lighttpd/core_lua.h>
# include <lualib.h>
# include <lauxlib.h>
#endif

#include <sched.h>

static liConnection* worker_con_get(liWorker *wrk);

/* closing sockets - wait for proper shutdown */

typedef struct worker_closing_socket worker_closing_socket;

struct worker_closing_socket {
	liWorker *wrk;
	GList *link;
	int fd;
	ev_tstamp close_timeout;
};

static void worker_close_socket_now(worker_closing_socket *scs) {
	liWorker *wrk = scs->wrk;

	shutdown(scs->fd, SHUT_RD);
	close(scs->fd);
	g_queue_delete_link(&wrk->closing_sockets, scs->link);
	g_slice_free(worker_closing_socket, scs);
}

static void worker_closing_socket_cb(int revents, void* arg) {
	worker_closing_socket *scs = (worker_closing_socket*) arg;
	liWorker *wrk = scs->wrk;
	ssize_t r;
	ev_tstamp remaining = scs->close_timeout - ev_now(wrk->loop);

	/* empty the input buffer, wait for EOF or timeout or a socket error to close it */
	g_string_set_size(wrk->tmp_str, 1024);
	for (;;) {
		r = read(scs->fd, wrk->tmp_str->str, wrk->tmp_str->len);
		if (0 == r) break; /* got EOF */
		if (0 > r) { /* error */
			switch (errno) {
			case EINTR:
				/* call read again */
				continue;
			case EAGAIN:
#if EWOULDBLOCK != EAGAIN
			case EWOULDBLOCK:
#endif
				/* check timeout: */
				if (remaining > 0 && !(revents & EV_TIMEOUT)) {
					/* wait again */
					ev_once(wrk->loop, scs->fd, EV_READ, remaining, worker_closing_socket_cb, scs);
					return;
				}
				/* timeout reached, break switch and loop */
				break;
			default:
				/* real error (probably ECONNRESET or similar): break switch and loop */
				/* no logging: there is no context anymore for the socket */
				break;
			}
			break; /* end loop */
		}
	}

	worker_close_socket_now(scs);
}

void li_worker_add_closing_socket(liWorker *wrk, int fd) {
	worker_closing_socket *scs;
	liServerState state = g_atomic_int_get(&wrk->srv->state);

	shutdown(fd, SHUT_WR);
	if (LI_SERVER_RUNNING != state && LI_SERVER_WARMUP != state) {
		shutdown(fd, SHUT_RD);
		close(fd);
		return;
	}

	scs = g_slice_new0(worker_closing_socket);
	scs->wrk = wrk;
	scs->fd = fd;
	g_queue_push_tail(&wrk->closing_sockets, scs);
	scs->link = g_queue_peek_tail_link(&wrk->closing_sockets);
	scs->close_timeout = ev_now(wrk->loop) + 10.0;

	ev_once(wrk->loop, fd, EV_READ, 10.0, worker_closing_socket_cb, scs);
}

/* Kill it - frees fd */
static void worker_rem_closing_socket(liWorker *wrk, worker_closing_socket *scs) {
	scs->close_timeout = ev_now(wrk->loop);
	ev_feed_fd_event(wrk->loop, scs->fd, EV_READ);
}

/* Keep alive */

void li_worker_check_keepalive(liWorker *wrk) {
	ev_tstamp now = ev_now(wrk->loop);

	if (0 == wrk->keep_alive_queue.length) {
		ev_timer_stop(wrk->loop, &wrk->keep_alive_timer);
	} else {
		wrk->keep_alive_timer.repeat = ((liConnection*)g_queue_peek_head(&wrk->keep_alive_queue))->keep_alive_data.timeout - now + 1;
		ev_timer_again(wrk->loop, &wrk->keep_alive_timer);
	}
}

static void worker_keepalive_cb(struct ev_loop *loop, ev_timer *w, int revents) {
	liWorker *wrk = (liWorker*) w->data;
	ev_tstamp now = ev_now(wrk->loop);
	GQueue *q = &wrk->keep_alive_queue;
	GList *l;
	liConnection *con;

	UNUSED(loop);
	UNUSED(revents);

	while ( NULL != (l = g_queue_peek_head_link(q)) &&
	        (con = (liConnection*) l->data)->keep_alive_data.timeout <= now ) {
		ev_tstamp remaining = con->keep_alive_data.max_idle - wrk->srv->keep_alive_queue_timeout - (now - con->keep_alive_data.timeout);
		if (remaining > 0) {
			g_queue_delete_link(q, l);
			con->keep_alive_data.link = NULL;
			ev_timer_set(&con->keep_alive_data.watcher, remaining, 0);
			ev_timer_start(wrk->loop, &con->keep_alive_data.watcher);
		} else {
			/* close it */
			li_worker_con_put(con);
		}
	}

	if (NULL == l) {
		ev_timer_stop(wrk->loop, &wrk->keep_alive_timer);
	} else {
		wrk->keep_alive_timer.repeat = con->keep_alive_data.timeout - now + 1;
		ev_timer_again(wrk->loop, &wrk->keep_alive_timer);
	}
}

/* check for timeouted connections */
static void worker_io_timeout_cb(struct ev_loop *loop, ev_timer *w, int revents) {
	liWorker *wrk = (liWorker*) w->data;
	liConnection *con;
	liWaitQueueElem *wqe;
	ev_tstamp now = CUR_TS(wrk);

	UNUSED(loop);
	UNUSED(revents);

	while ((wqe = li_waitqueue_pop(&wrk->io_timeout_queue)) != NULL) {
		liVRequest *vr;
		/* connection has timed out */
		con = wqe->data;
		vr = con->mainvr;
		if (CORE_OPTION(LI_CORE_OPTION_DEBUG_REQUEST_HANDLING).boolean) {
			VR_DEBUG(vr, "connection io-timeout from %s after %.2f seconds", con->remote_addr_str->str, now - wqe->ts);
		}
		li_plugins_handle_close(con);
		li_worker_con_put(con);
	}

	li_waitqueue_update(&wrk->io_timeout_queue);
}

/* run vreqest state machine */
static void worker_job_queue_cb(struct ev_loop *loop, ev_timer *w, int revents) {
	liWorker *wrk = (liWorker*) w->data;
	GQueue q = wrk->job_queue;
	GList *l;
	liVRequest *vr;
	UNUSED(loop);
	UNUSED(revents);

	g_queue_init(&wrk->job_queue); /* reset queue, elements are in q */

	while (NULL != (l = g_queue_pop_head_link(&q))) {
		vr = l->data;
		g_assert(g_atomic_int_compare_and_exchange(&vr->queued, 1, 0));
		li_vrequest_state_machine(vr);
	}
}

/* run vreqest state machine for async queued jobs */
static void worker_job_async_queue_cb(struct ev_loop *loop, ev_async *w, int revents) {
	liWorker *wrk = (liWorker*) w->data;
	GAsyncQueue *q = wrk->job_async_queue;
	liVRequestRef *vr_ref;
	liVRequest *vr;
	UNUSED(loop);
	UNUSED(revents);

	while (NULL != (vr_ref = g_async_queue_try_pop(q))) {
		if (NULL != (vr = li_vrequest_release_ref(vr_ref))) {
			g_assert(g_atomic_int_compare_and_exchange(&vr->queued, 1, 0));
			li_vrequest_state_machine(vr);
		}
	}
}


/* cache timestamp */
GString *li_worker_current_timestamp(liWorker *wrk, liTimeFunc timefunc, guint format_ndx) {
	gsize len;
	struct tm tm;
	liWorkerTS *wts;
	time_t now = (time_t)CUR_TS(wrk);

	if (timefunc == LI_GMTIME)
		wts = &g_array_index(wrk->timestamps_gmt, liWorkerTS, format_ndx);
	else
		wts = &g_array_index(wrk->timestamps_local, liWorkerTS, format_ndx);

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

/* stop worker watcher */
static void li_worker_stop_cb(struct ev_loop *loop, ev_async *w, int revents) {
	liWorker *wrk = (liWorker*) w->data;
	UNUSED(loop);
	UNUSED(revents);

	li_worker_stop(wrk, wrk);
}

/* stop worker watcher */
static void li_worker_suspend_cb(struct ev_loop *loop, ev_async *w, int revents) {
	liWorker *wrk = (liWorker*) w->data;
	UNUSED(loop);
	UNUSED(revents);

	li_worker_suspend(wrk, wrk);
}

/* exit worker watcher */
static void li_worker_exit_cb(struct ev_loop *loop, ev_async *w, int revents) {
	liWorker *wrk = (liWorker*) w->data;
	UNUSED(loop);
	UNUSED(revents);

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

		con->srv_sock = srv_sock;
		con->state = LI_CON_STATE_REQUEST_START;
		ev_io_set(&con->sock_watcher, s, EV_READ);
		ev_io_start(wrk->loop, &con->sock_watcher);
		con->ts = CUR_TS(con->wrk);

		con->remote_addr = remote_addr;
		li_sockaddr_to_string(remote_addr, con->remote_addr_str, FALSE);

		con->local_addr = li_sockaddr_local_from_socket(s);
		li_sockaddr_to_string(con->local_addr, con->local_addr_str, FALSE);

		li_waitqueue_push(&wrk->io_timeout_queue, &con->io_timeout_elem);


		if (srv_sock->new_cb) {
			if (!srv_sock->new_cb(con)) {
				li_connection_error(con);
			}
		}
	} else {
		li_worker_new_con_data *d = g_slice_new(li_worker_new_con_data);
		d->remote_addr = remote_addr;
		d->s = s;
		d->srv_sock = srv_sock;
		g_async_queue_push(wrk->new_con_queue, d);
		ev_async_send(wrk->loop, &wrk->new_con_watcher);
	}
}

static void li_worker_new_con_cb(struct ev_loop *loop, ev_async *w, int revents) {
	liWorker *wrk = (liWorker*) w->data;
	li_worker_new_con_data *d;
	UNUSED(loop);
	UNUSED(revents);

	while (NULL != (d = g_async_queue_try_pop(wrk->new_con_queue))) {
		li_worker_new_con(wrk, wrk, d->remote_addr, d->s, d->srv_sock);
		g_slice_free(li_worker_new_con_data, d);
	}
}

/* stats watcher */
static void worker_stats_watcher_cb(struct ev_loop *loop, ev_timer *w, int revents) {
	liWorker *wrk = (liWorker*) w->data;
	ev_tstamp now = ev_now(wrk->loop);
	UNUSED(loop);
	UNUSED(revents);

	if (wrk->stats.last_update && now != wrk->stats.last_update) {
		wrk->stats.requests_per_sec =
			(wrk->stats.requests - wrk->stats.last_requests) / (now - wrk->stats.last_update);
#if 0
			if (wrk->stats.requests_per_sec > 0)
			DEBUG(wrk->srv, "worker %u: %.2f requests per second", wrk->ndx, wrk->stats.requests_per_sec);
#endif
	}

	/* 5s averages */
	if ((now - wrk->stats.last_avg) > 5) {
		/* bytes in */
		wrk->stats.bytes_in_5s_diff = wrk->stats.bytes_in - wrk->stats.bytes_in_5s;
		wrk->stats.bytes_in_5s = wrk->stats.bytes_in;

		/* bytes out */
		wrk->stats.bytes_out_5s_diff = wrk->stats.bytes_out - wrk->stats.bytes_out_5s;
		wrk->stats.bytes_out_5s = wrk->stats.bytes_out;

		/* requests */
		wrk->stats.requests_5s_diff = wrk->stats.requests - wrk->stats.requests_5s;
		wrk->stats.requests_5s = wrk->stats.requests;

		/* active connections */
		wrk->stats.active_cons_5s = wrk->connections_active;

		wrk->stats.last_avg = now;
	}

	wrk->stats.active_cons_cum += wrk->connections_active;

	wrk->stats.last_requests = wrk->stats.requests;
	wrk->stats.last_update = now;
}

/* init */

liWorker* li_worker_new(liServer *srv, struct ev_loop *loop) {
	liWorker *wrk = g_slice_new0(liWorker);
	wrk->srv = srv;
	wrk->loop = loop;

#ifdef HAVE_LUA_H
	wrk->L = luaL_newstate();
	luaL_openlibs(wrk->L);
	li_lua_init(wrk->L, srv, wrk);
#else
	wrk->L = NULL;
#endif

	g_queue_init(&wrk->keep_alive_queue);
	ev_init(&wrk->keep_alive_timer, worker_keepalive_cb);
	wrk->keep_alive_timer.data = wrk;

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

	ev_init(&wrk->li_worker_exit_watcher, li_worker_exit_cb);
	wrk->li_worker_exit_watcher.data = wrk;
	ev_async_start(wrk->loop, &wrk->li_worker_exit_watcher);
	ev_unref(wrk->loop); /* this watcher shouldn't keep the loop alive */

	ev_init(&wrk->li_worker_stop_watcher, li_worker_stop_cb);
	wrk->li_worker_stop_watcher.data = wrk;
	ev_async_start(wrk->loop, &wrk->li_worker_stop_watcher);

	ev_init(&wrk->li_worker_suspend_watcher, li_worker_suspend_cb);
	wrk->li_worker_suspend_watcher.data = wrk;
	ev_async_start(wrk->loop, &wrk->li_worker_suspend_watcher);

	ev_init(&wrk->new_con_watcher, li_worker_new_con_cb);
	wrk->new_con_watcher.data = wrk;
	ev_async_start(wrk->loop, &wrk->new_con_watcher);
	wrk->new_con_queue = g_async_queue_new();

	ev_timer_init(&wrk->stats_watcher, worker_stats_watcher_cb, 1, 1);
	wrk->stats_watcher.data = wrk;
	ev_timer_start(wrk->loop, &wrk->stats_watcher);
	ev_unref(wrk->loop); /* this watcher shouldn't keep the loop alive */

	ev_init(&wrk->collect_watcher, li_collect_watcher_cb);
	wrk->collect_watcher.data = wrk;
	ev_async_start(wrk->loop, &wrk->collect_watcher);
	wrk->collect_queue = g_async_queue_new();
	ev_unref(wrk->loop); /* this watcher shouldn't keep the loop alive */

	/* io timeout timer */
	li_waitqueue_init(&wrk->io_timeout_queue, wrk->loop, worker_io_timeout_cb, srv->io_timeout, wrk);

	/* throttling */
	li_waitqueue_init(&wrk->throttle_queue, wrk->loop, li_throttle_cb, THROTTLE_GRANULARITY, wrk);

	/* job queue */
	g_queue_init(&wrk->job_queue);
	ev_timer_init(&wrk->job_queue_watcher, worker_job_queue_cb, 0, 0);
	wrk->job_queue_watcher.data = wrk;

	wrk->job_async_queue = g_async_queue_new();
	ev_async_init(&wrk->job_async_queue_watcher, worker_job_async_queue_cb);
	wrk->job_async_queue_watcher.data = wrk;
	ev_async_start(wrk->loop, &wrk->job_async_queue_watcher);
	ev_unref(wrk->loop); /* this watcher shouldn't keep the loop alive */

	li_stat_cache_new(wrk, srv->stat_cache_ttl);

	return wrk;
}

void li_worker_free(liWorker *wrk) {
	if (!wrk) return;

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
			li_connection_free(g_array_index(wrk->connections, liConnection*, i));
		}
		g_array_free(wrk->connections, TRUE);
	}

	{ /* force closing sockets */
		GList *iter;
		for (iter = g_queue_peek_head_link(&wrk->closing_sockets); iter; iter = g_list_next(iter)) {
			worker_close_socket_now((worker_closing_socket*) iter->data);
		}
		g_queue_clear(&wrk->closing_sockets);
	}

	li_ev_safe_ref_and_stop(ev_async_stop, wrk->loop, &wrk->job_async_queue_watcher);

	{ /* free timestamps */
		guint i;
		for (i = 0; i < wrk->timestamps_gmt->len; i++) {
			g_string_free(g_array_index(wrk->timestamps_gmt, liWorkerTS, i).str, TRUE);
			g_string_free(g_array_index(wrk->timestamps_local, liWorkerTS, i).str, TRUE);
		}
		g_array_free(wrk->timestamps_gmt, TRUE);
		g_array_free(wrk->timestamps_local, TRUE);
	}

	li_ev_safe_ref_and_stop(ev_async_stop, wrk->loop, &wrk->li_worker_exit_watcher);

	{
		GAsyncQueue *q = wrk->job_async_queue;
		liVRequestRef *vr_ref;
		liVRequest *vr;

		while (NULL != (vr_ref = g_async_queue_try_pop(q))) {
			if (NULL != (vr = li_vrequest_release_ref(vr_ref))) {
				g_assert(g_atomic_int_compare_and_exchange(&vr->queued, 1, 0));
				li_vrequest_state_machine(vr);
			}
		}

		g_async_queue_unref(q);
	}


	g_async_queue_unref(wrk->new_con_queue);

	li_ev_safe_ref_and_stop(ev_timer_stop, wrk->loop, &wrk->stats_watcher);

	li_ev_safe_ref_and_stop(ev_async_stop, wrk->loop, &wrk->collect_watcher);
	li_collect_watcher_cb(wrk->loop, &wrk->collect_watcher, 0);
	g_async_queue_unref(wrk->collect_queue);

	g_string_free(wrk->tmp_str, TRUE);

	li_stat_cache_free(wrk->stat_cache);

#ifdef HAVE_LUA_H
	lua_close(wrk->L);
	wrk->L = NULL;
#endif

	g_slice_free(liWorker, wrk);
}

void li_worker_run(liWorker *wrk) {
#ifdef LIGHTY_OS_LINUX
	/* sched_setaffinity is only available on linux */
	cpu_set_t mask;

	if (0 != sched_getaffinity(0, sizeof(mask), &mask)) {
		ERROR(wrk->srv, "couldn't get cpu affinity mask: %s", g_strerror(errno));
	} else {
		guint cpus = 0;
		while (CPU_ISSET(cpus, &mask)) cpus++;
		if (cpus) {
			CPU_ZERO(&mask);
			CPU_SET(wrk->ndx % cpus, &mask);
			if (0 != sched_setaffinity(0, sizeof(mask), &mask)) {
				ERROR(wrk->srv, "couldn't set cpu affinity mask: %s", g_strerror(errno));
			}
		} else {
			ERROR(wrk->srv, "%s", "cpu 0 not enabled, no affinity set");
		}
	}
#endif
	
	ev_loop(wrk->loop, 0);
}

void li_worker_stop(liWorker *context, liWorker *wrk) {
	if (context == wrk) {
		guint i;

		ev_async_stop(wrk->loop, &wrk->li_worker_stop_watcher);
		ev_async_stop(wrk->loop, &wrk->li_worker_suspend_watcher);
		ev_async_stop(wrk->loop, &wrk->new_con_watcher);
		li_waitqueue_stop(&wrk->io_timeout_queue);
		li_waitqueue_stop(&wrk->throttle_queue);
		li_waitqueue_stop(&wrk->stat_cache->delete_queue);
		li_worker_new_con_cb(wrk->loop, &wrk->new_con_watcher, 0); /* handle remaining new connections */

		/* close keep alive connections */
		for (i = wrk->connections_active; i-- > 0;) {
			liConnection *con = g_array_index(wrk->connections, liConnection*, i);
			if (con->state == LI_CON_STATE_KEEP_ALIVE) {
				li_worker_con_put(con);
			}
		}

		li_worker_check_keepalive(wrk);

		{ /* force closing sockets */
			GList *iter;
			for (iter = g_queue_peek_head_link(&wrk->closing_sockets); iter; iter = g_list_next(iter)) {
				worker_rem_closing_socket(wrk, (worker_closing_socket*) iter->data);
			}
		}
	} else {
		ev_async_send(wrk->loop, &wrk->li_worker_stop_watcher);
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

		{ /* force closing sockets */
			GList *iter;
			for (iter = g_queue_peek_head_link(&wrk->closing_sockets); iter; iter = g_list_next(iter)) {
				worker_rem_closing_socket(wrk, (worker_closing_socket*) iter->data);
			}
		}

#if 0
		ERROR(wrk->srv, "%i connections still active", (int) wrk->connections_active);
		for (i = wrk->connections_active; i-- > 0;) {
			liConnection *con = g_array_index(wrk->connections, liConnection*, i);
			ERROR(wrk->srv, "%i state: %s, %i", i, li_connection_state_str(con->state), con->mainvr->state);
		}
#endif
	} else {
		ev_async_send(wrk->loop, &wrk->li_worker_suspend_watcher);
	}
}

void li_worker_exit(liWorker *context, liWorker *wrk) {
	if (context == wrk) {
		ev_unloop (wrk->loop, EVUNLOOP_ALL);
	} else {
		ev_async_send(wrk->loop, &wrk->li_worker_exit_watcher);
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
	}
	g_atomic_int_inc((gint*) &wrk->connections_active);
	return con;
}

void li_worker_con_put(liConnection *con) {
	guint threshold;
	liWorker *wrk = con->wrk;
	ev_tstamp now = CUR_TS(wrk);
	
	if (con->state == LI_CON_STATE_DEAD)
		/* already disconnected */
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
		con->idx = wrk->connections_active;
		g_array_index(wrk->connections, liConnection*, con->idx) = con;
		g_array_index(wrk->connections, liConnection*, tmp->idx) = tmp;
	}

	li_connection_reset(con);

	/* realloc wrk->connections if it makes sense (too many allocated, only every 60sec) */
	/* if (active < allocated*0.70) { allocated *= 0.85 } */
	threshold = (wrk->connections->len * 7) / 10;
	if (wrk->connections_active < threshold && (now - wrk->connections_gc_ts) > 60.0 && wrk->connections->len > 10) {
		/* realloc */
		guint i;
		threshold = (wrk->connections->len * 85) / 100;
		for (i = wrk->connections->len; i > threshold; i--) {
			li_connection_free(g_array_index(wrk->connections, liConnection*, i-1));
			g_array_index(wrk->connections, liConnection*, i-1) = NULL;
		}
		wrk->connections->len = threshold;
		wrk->connections_gc_ts = now;
	}
}

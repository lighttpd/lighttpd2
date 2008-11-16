
#include <sched.h>

#include <lighttpd/base.h>

static connection* worker_con_get(worker *wrk);
void worker_con_put(connection *con);

/* closing sockets - wait for proper shutdown */

struct worker_closing_socket;
typedef struct worker_closing_socket worker_closing_socket;

struct worker_closing_socket {
	worker *wrk;
	GList *link;
	int fd;
};

static void worker_closing_socket_cb(int revents, void* arg) {
	worker_closing_socket *scs = (worker_closing_socket*) arg;
	UNUSED(revents);

	/* Whatever happend: we just close the socket */
	shutdown(scs->fd, SHUT_RD);
	close(scs->fd);
	g_queue_delete_link(&scs->wrk->closing_sockets, scs->link);
	g_slice_free(worker_closing_socket, scs);
}

void worker_add_closing_socket(worker *wrk, int fd) {
	worker_closing_socket *scs;

	shutdown(fd, SHUT_WR);
	if (g_atomic_int_get(&wrk->srv->state) == SERVER_STOPPING) {
		shutdown(fd, SHUT_RD);
		close(fd);
		return;
	}

	scs = g_slice_new0(worker_closing_socket);
	scs->wrk = wrk;
	scs->fd = fd;
	g_queue_push_tail(&wrk->closing_sockets, scs);
	scs->link = g_queue_peek_tail_link(&wrk->closing_sockets);

	ev_once(wrk->loop, fd, EV_READ, 10.0, worker_closing_socket_cb, scs);
}

/* Kill it - frees fd */
static void worker_rem_closing_socket(worker *wrk, worker_closing_socket *scs) {
	ev_feed_fd_event(wrk->loop, scs->fd, EV_READ);
}

/* Keep alive */

void worker_check_keepalive(worker *wrk) {
	ev_tstamp now = ev_now(wrk->loop);

	if (0 == wrk->keep_alive_queue.length) {
		ev_timer_stop(wrk->loop, &wrk->keep_alive_timer);
	} else {
		wrk->keep_alive_timer.repeat = ((connection*)g_queue_peek_head(&wrk->keep_alive_queue))->keep_alive_data.timeout - now + 1;
		ev_timer_again(wrk->loop, &wrk->keep_alive_timer);
	}
}

static void worker_keepalive_cb(struct ev_loop *loop, ev_timer *w, int revents) {
	worker *wrk = (worker*) w->data;
	ev_tstamp now = ev_now(wrk->loop);
	GQueue *q = &wrk->keep_alive_queue;
	GList *l;
	connection *con;

	UNUSED(loop);
	UNUSED(revents);

	while ( NULL != (l = g_queue_peek_head_link(q)) &&
	        (con = (connection*) l->data)->keep_alive_data.timeout <= now ) {
		ev_tstamp remaining = con->keep_alive_data.max_idle - wrk->srv->keep_alive_queue_timeout - (now - con->keep_alive_data.timeout);
		if (remaining > 0) {
			g_queue_delete_link(q, l);
			con->keep_alive_data.link = NULL;
			ev_timer_set(&con->keep_alive_data.watcher, remaining, 0);
			ev_timer_start(wrk->loop, &con->keep_alive_data.watcher);
		} else {
			/* close it */
			worker_con_put(con);
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
	worker *wrk = (worker*) w->data;
	connection *con;
	waitqueue_elem *wqe;
	ev_tstamp now = CUR_TS(wrk);

	UNUSED(loop);
	UNUSED(revents);

	while ((wqe = waitqueue_pop(&wrk->io_timeout_queue)) != NULL) {
		/* connection has timed out */
		con = wqe->data;
		CON_TRACE(con, "connection io-timeout from %s after %.2f seconds", con->remote_addr_str->str, now - wqe->ts);
		plugins_handle_close(con);
		worker_con_put(con);
	}

	waitqueue_update(&wrk->io_timeout_queue);
}

/* check for throttled connections */
static void worker_throttle_cb(struct ev_loop *loop, ev_timer *w, int revents) {
	worker *wrk = (worker*) w->data;
	connection *con;
	waitqueue_elem *wqe;

	UNUSED(revents);

	while ((wqe = waitqueue_pop(&wrk->throttle_queue)) != NULL) {
		/* connection waited long enough to reenable sending of data again */
		con = wqe->data;
		ev_io_add_events(loop, &con->sock_watcher, EV_WRITE);
	}

	waitqueue_update(&wrk->throttle_queue);
}

/* cache timestamp */
GString *worker_current_timestamp(worker *wrk) {
	time_t cur_ts = (time_t)CUR_TS(wrk);
	if (cur_ts != wrk->last_generated_date_ts) {
		g_string_set_size(wrk->ts_date_str, 255);
		strftime(wrk->ts_date_str->str, wrk->ts_date_str->allocated_len,
				"%a, %d %b %Y %H:%M:%S GMT", gmtime(&(cur_ts)));

		g_string_set_size(wrk->ts_date_str, strlen(wrk->ts_date_str->str));

		wrk->last_generated_date_ts = cur_ts;
	}
	return wrk->ts_date_str;
}

/* stop worker watcher */
static void worker_stop_cb(struct ev_loop *loop, ev_async *w, int revents) {
	UNUSED(loop);
	UNUSED(revents);
	worker *wrk = (worker*) w->data;
	worker_stop(wrk, wrk);
}

/* exit worker watcher */
static void worker_exit_cb(struct ev_loop *loop, ev_async *w, int revents) {
	UNUSED(loop);
	UNUSED(revents);
	worker *wrk = (worker*) w->data;
	worker_exit(wrk, wrk);
}

struct worker_new_con_data;
typedef struct worker_new_con_data worker_new_con_data;
struct worker_new_con_data {
	sock_addr remote_addr;
	int s;
};

/* new con watcher */
void worker_new_con(worker *ctx, worker *wrk, sock_addr *remote_addr, int s) {
	if (ctx == wrk) {
		connection *con = worker_con_get(wrk);

		con->state = CON_STATE_REQUEST_START;
		con->remote_addr = *remote_addr;
		ev_io_set(&con->sock_watcher, s, EV_READ);
		ev_io_start(wrk->loop, &con->sock_watcher);
		con->ts = CUR_TS(con->wrk);
		sockaddr_to_string(remote_addr, con->remote_addr_str);
		waitqueue_push(&wrk->io_timeout_queue, &con->io_timeout_elem);
	} else {
		worker_new_con_data *d = g_slice_new(worker_new_con_data);
		d->remote_addr = *remote_addr;
		d->s = s;
		g_async_queue_push(wrk->new_con_queue, d);
		ev_async_send(wrk->loop, &wrk->new_con_watcher);
	}
}

static void worker_new_con_cb(struct ev_loop *loop, ev_async *w, int revents) {
	worker *wrk = (worker*) w->data;
	worker_new_con_data *d;
	UNUSED(loop);
	UNUSED(revents);

	while (NULL != (d = g_async_queue_try_pop(wrk->new_con_queue))) {
		worker_new_con(wrk, wrk, &d->remote_addr, d->s);
		g_slice_free(worker_new_con_data, d);
	}
}

/* stats watcher */
static void worker_stats_watcher_cb(struct ev_loop *loop, ev_timer *w, int revents) {
	worker *wrk = (worker*) w->data;
	UNUSED(loop);
	UNUSED(revents);

	ev_tstamp now = ev_now(wrk->loop);

	if (wrk->stats.last_update && now != wrk->stats.last_update) {
		wrk->stats.requests_per_sec =
			(wrk->stats.requests - wrk->stats.last_requests) / (now - wrk->stats.last_update);
		if (wrk->stats.requests_per_sec > 0)
			TRACE(wrk->srv, "worker %u: %.2f requests per second", wrk->ndx, wrk->stats.requests_per_sec);
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

worker* worker_new(struct server *srv, struct ev_loop *loop) {
	worker *wrk = g_slice_new0(worker);
	wrk->srv = srv;
	wrk->loop = loop;

	g_queue_init(&wrk->keep_alive_queue);
	ev_init(&wrk->keep_alive_timer, worker_keepalive_cb);
	wrk->keep_alive_timer.data = wrk;

	wrk->connections_active = 0;
	wrk->connections = g_array_new(FALSE, TRUE, sizeof(connection*));

	wrk->tmp_str = g_string_sized_new(255);

	wrk->last_generated_date_ts = 0;
	wrk->ts_date_str = g_string_sized_new(255);

	ev_init(&wrk->worker_exit_watcher, worker_exit_cb);
	wrk->worker_exit_watcher.data = wrk;
	ev_async_start(wrk->loop, &wrk->worker_exit_watcher);
	ev_unref(wrk->loop); /* this watcher shouldn't keep the loop alive */

	ev_init(&wrk->worker_stop_watcher, worker_stop_cb);
	wrk->worker_stop_watcher.data = wrk;
	ev_async_start(wrk->loop, &wrk->worker_stop_watcher);

	ev_init(&wrk->new_con_watcher, worker_new_con_cb);
	wrk->new_con_watcher.data = wrk;
	ev_async_start(wrk->loop, &wrk->new_con_watcher);
	wrk->new_con_queue = g_async_queue_new();

	ev_timer_init(&wrk->stats_watcher, worker_stats_watcher_cb, 1, 1);
	wrk->stats_watcher.data = wrk;
	ev_timer_start(wrk->loop, &wrk->stats_watcher);
	ev_unref(wrk->loop); /* this watcher shouldn't keep the loop alive */

	ev_init(&wrk->collect_watcher, collect_watcher_cb);
	wrk->collect_watcher.data = wrk;
	ev_async_start(wrk->loop, &wrk->collect_watcher);
	wrk->collect_queue = g_async_queue_new();
	ev_unref(wrk->loop); /* this watcher shouldn't keep the loop alive */

	/* io timeout timer */
	waitqueue_init(&wrk->io_timeout_queue, wrk->loop, worker_io_timeout_cb, srv->io_timeout, wrk);
	ev_unref(wrk->loop); /* this watcher shouldn't keep the loop alive */

	/* throttling */
	waitqueue_init(&wrk->throttle_queue, wrk->loop, worker_throttle_cb, 0.5, wrk);
	ev_unref(wrk->loop); /* this watcher shouldn't keep the loop alive */

	return wrk;
}

void worker_free(worker *wrk) {
	if (!wrk) return;

	{ /* close connections */
		guint i;
		if (wrk->connections_active > 0) {
			ERROR(wrk->srv, "Server shutdown with unclosed connections: %u", wrk->connections_active);
			for (i = wrk->connections_active; i-- > 0;) {
				connection *con = g_array_index(wrk->connections, connection*, i);
				connection_error(con);
			}
		}
		for (i = 0; i < wrk->connections->len; i++) {
			connection_free(g_array_index(wrk->connections, connection*, i));
		}
		g_array_free(wrk->connections, TRUE);
	}

	{ /* force closing sockets */
		GList *iter;
		for (iter = g_queue_peek_head_link(&wrk->closing_sockets); iter; iter = g_list_next(iter)) {
			worker_closing_socket_cb(EV_TIMEOUT, (worker_closing_socket*) iter->data);
		}
		g_queue_clear(&wrk->closing_sockets);
	}

	ev_ref(wrk->loop);
	ev_async_stop(wrk->loop, &wrk->worker_exit_watcher);

	g_string_free(wrk->tmp_str, TRUE);
	g_string_free(wrk->ts_date_str, TRUE);

	g_async_queue_unref(wrk->new_con_queue);

	ev_ref(wrk->loop);
	ev_timer_stop(wrk->loop, &wrk->stats_watcher);

	ev_ref(wrk->loop);
	ev_async_stop(wrk->loop, &wrk->collect_watcher);
	collect_watcher_cb(wrk->loop, &wrk->collect_watcher, 0);
	g_async_queue_unref(wrk->collect_queue);

	g_slice_free(worker, wrk);
}

void worker_run(worker *wrk) {
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
	ev_loop(wrk->loop, 0);
}

void worker_stop(worker *context, worker *wrk) {
	if (context == wrk) {
		guint i;

		ev_async_stop(wrk->loop, &wrk->worker_stop_watcher);
		ev_async_stop(wrk->loop, &wrk->new_con_watcher);
		worker_new_con_cb(wrk->loop, &wrk->new_con_watcher, 0); /* handle remaining new connections */

		/* close keep alive connections */
		for (i = wrk->connections_active; i-- > 0;) {
			connection *con = g_array_index(wrk->connections, connection*, i);
			if (con->state == CON_STATE_KEEP_ALIVE)
				worker_con_put(con);
		}

		worker_check_keepalive(wrk);

		{ /* force closing sockets */
			GList *iter;
			for (iter = g_queue_peek_head_link(&wrk->closing_sockets); iter; iter = g_list_next(iter)) {
				worker_rem_closing_socket(wrk, (worker_closing_socket*) iter->data);
			}
		}
	} else {
		ev_async_send(wrk->loop, &wrk->worker_stop_watcher);
	}
}

void worker_exit(worker *context, worker *wrk) {
	if (context == wrk) {
		ev_unloop (wrk->loop, EVUNLOOP_ALL);
	} else {
		ev_async_send(wrk->loop, &wrk->worker_exit_watcher);
	}
}


static connection* worker_con_get(worker *wrk) {
	connection *con;

	if (wrk->connections_active >= wrk->connections->len) {
		con = connection_new(wrk);
		con->idx = wrk->connections_active;
		g_array_append_val(wrk->connections, con);
	} else {
		con = g_array_index(wrk->connections, connection*, wrk->connections_active);
	}
	g_atomic_int_inc((gint*) &wrk->connections_active);
	return con;
}

void worker_con_put(connection *con) {
	worker *wrk = con->wrk;

	connection_reset(con);
	g_atomic_int_add((gint*) &wrk->connection_load, -1);
	g_atomic_int_add((gint*) &wrk->connections_active, -1);
	if (con->idx != wrk->connections_active) {
		/* Swap [con->idx] and [wrk->connections_active] */
		connection *tmp;
		assert(con->idx < wrk->connections_active); /* con must be an active connection */
		tmp = g_array_index(wrk->connections, connection*, wrk->connections_active);
		tmp->idx = con->idx;
		con->idx = wrk->connections_active;
		g_array_index(wrk->connections, connection*, con->idx) = con;
		g_array_index(wrk->connections, connection*, tmp->idx) = tmp;
	}
}

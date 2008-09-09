
#include "base.h"

void con_put(connection *con);

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
	worker_closing_socket *scs = g_slice_new0(worker_closing_socket);

	shutdown(fd, SHUT_WR);

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
			con_put(con);
		}
	}

	if (NULL == l) {
		ev_timer_stop(wrk->loop, &wrk->keep_alive_timer);
	} else {
		wrk->keep_alive_timer.repeat = con->keep_alive_data.timeout - now + 1;
		ev_timer_again(wrk->loop, &wrk->keep_alive_timer);
	}
}

/* cache timestamp */
GString *worker_current_timestamp(worker *wrk) {
	time_t cur_ts = CUR_TS(wrk);
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

/* new con watcher */
void worker_new_con(worker *wrk, connection *con) {
	if (wrk == con->wrk) {
		ev_io_start(wrk->loop, &con->sock_watcher);
	} else {
		wrk = con->wrk;
		g_async_queue_push(wrk->new_con_queue, con);
		ev_async_send(wrk->loop, &wrk->new_con_watcher);
	}
}

static void worker_new_con_cb(struct ev_loop *loop, ev_async *w, int revents) {
	worker *wrk = (worker*) w->data;
	connection *con;
	UNUSED(loop);
	UNUSED(revents);

	while (NULL != (con = g_async_queue_try_pop(wrk->new_con_queue))) {
		worker_new_con(wrk, con);
	}
}

/* init */

worker* worker_new(struct server *srv, struct ev_loop *loop) {
	worker *wrk = g_slice_new0(worker);
	wrk->srv = srv;
	wrk->loop = loop;

	g_queue_init(&wrk->keep_alive_queue);
	ev_init(&wrk->keep_alive_timer, worker_keepalive_cb);
	wrk->keep_alive_timer.data = wrk;

	wrk->tmp_str = g_string_sized_new(255);

	wrk->last_generated_date_ts = 0;
	wrk->ts_date_str = g_string_sized_new(255);

	ev_init(&wrk->worker_exit_watcher, worker_exit_cb);
	wrk->worker_exit_watcher.data = wrk;
	ev_async_start(wrk->loop, &wrk->worker_exit_watcher);
	ev_unref(wrk->loop); /* this watcher shouldn't keep the loop alive; it is never stopped */

	ev_init(&wrk->worker_stop_watcher, worker_stop_cb);
	wrk->worker_stop_watcher.data = wrk;
	ev_async_start(wrk->loop, &wrk->worker_stop_watcher);

	ev_init(&wrk->new_con_watcher, worker_new_con_cb);
	wrk->new_con_watcher.data = wrk;
	ev_async_start(wrk->loop, &wrk->new_con_watcher);
	wrk->new_con_queue = g_async_queue_new();

	return wrk;
}

void worker_free(worker *wrk) {
	if (!wrk) return;

	{ /* force closing sockets */
		GList *iter;
		for (iter = g_queue_peek_head_link(&wrk->closing_sockets); iter; iter = g_list_next(iter)) {
			worker_closing_socket_cb(EV_TIMEOUT, (worker_closing_socket*) iter->data);
		}
		g_queue_clear(&wrk->closing_sockets);
	}

	g_string_free(wrk->tmp_str, TRUE);
	g_string_free(wrk->ts_date_str, TRUE);

	g_slice_free(worker, wrk);
}

void worker_run(worker *wrk) {
	ev_loop(wrk->loop, 0);
}

void worker_stop(worker *context, worker *wrk) {
	if (context == wrk) {
		guint i;
		server *srv = wrk->srv;

		ev_async_stop(wrk->loop, &wrk->worker_stop_watcher);
		ev_async_stop(wrk->loop, &wrk->new_con_watcher);

		WORKER_LOCK(srv, &srv->lock_con);
		for (i = srv->connections_active; i-- > 0;) {
			connection *con = g_array_index(srv->connections, connection*, i);
			if (con->wrk == wrk && con->state == CON_STATE_KEEP_ALIVE)
				con_put(con);
		}
		WORKER_UNLOCK(srv, &srv->lock_con);
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

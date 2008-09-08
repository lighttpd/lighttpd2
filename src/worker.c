
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
/*
static void worker_rem_closing_socket(worker *wrk, worker_closing_socket *scs) {
	ev_feed_fd_event(wrk->loop, scs->fd, EV_READ);
}
*/

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

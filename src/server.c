
#include "base.h"
#include "utils.h"
#include "plugin_core.h"

struct server_closing_socket;
typedef struct server_closing_socket server_closing_socket;

struct server_closing_socket {
	server *srv;
	GList *link;
	int fd;
};

static void server_closing_socket_cb(int revents, void* arg) {
	server_closing_socket *scs = (server_closing_socket*) arg;
	UNUSED(revents);

	/* Whatever happend: we just close the socket */
	shutdown(scs->fd, SHUT_RD);
	close(scs->fd);
	g_queue_delete_link(&scs->srv->closing_sockets, scs->link);
	g_slice_free(server_closing_socket, scs);
}

void server_add_closing_socket(server *srv, int fd) {
	server_closing_socket *scs = g_slice_new0(server_closing_socket);

	shutdown(fd, SHUT_WR);

	scs->srv = srv;
	scs->fd = fd;
	g_queue_push_tail(&srv->closing_sockets, scs);
	scs->link = g_queue_peek_tail_link(&srv->closing_sockets);

	ev_once(srv->loop, fd, EV_READ, 10.0, server_closing_socket_cb, scs);
}

/* Kill it - frees fd */
static void server_rem_closing_socket(server *srv, server_closing_socket *scs) {
	ev_feed_fd_event(srv->loop, scs->fd, EV_READ);
}

void con_put(server *srv, connection *con);

static void server_option_free(gpointer _so) {
	g_slice_free(server_option, _so);
}

static void server_action_free(gpointer _sa) {
	g_slice_free(server_action, _sa);
}

static void server_setup_free(gpointer _ss) {
	g_slice_free(server_setup, _ss);
}

static struct ev_signal
	sig_w_INT,
	sig_w_TERM,
	sig_w_PIPE;

static void sigint_cb(struct ev_loop *loop, struct ev_signal *w, int revents) {
	server *srv = (server*) w->data;
	UNUSED(revents);

	if (!srv->exiting) {
		INFO(srv, "Got signal, shutdown");
		server_exit(srv);
	} else {
		INFO(srv, "Got second signal, force shutdown");
		ev_unloop (loop, EVUNLOOP_ALL);
	}
}

static void sigpipe_cb(struct ev_loop *loop, struct ev_signal *w, int revents) {
	/* ignore */
	UNUSED(loop); UNUSED(w); UNUSED(revents);
}

#define CATCH_SIGNAL(loop, cb, n) do {\
	my_ev_init(&sig_w_##n, cb); \
	ev_signal_set(&sig_w_##n, SIG##n); \
	ev_signal_start(loop, &sig_w_##n); \
	sig_w_##n.data = srv; \
	ev_unref(loop); /* Signal watchers shouldn't keep loop alive */ \
} while (0)

void server_check_keepalive(server *srv) {
	ev_tstamp now = ev_now((srv)->loop);

	if (0 == srv->keep_alive_queue.length) {
		ev_timer_stop(srv->loop, &srv->keep_alive_timer);
	} else {
		srv->keep_alive_timer.repeat = ((connection*)g_queue_peek_head(&srv->keep_alive_queue))->keep_alive_data.timeout - now + 1;
		ev_timer_again(srv->loop, &srv->keep_alive_timer);
	}
}

static void server_keepalive_cb(struct ev_loop *loop, ev_timer *w, int revents) {
	server *srv = (server*) w->data;
	ev_tstamp now = ev_now((srv)->loop);
	GQueue *q = &srv->keep_alive_queue;
	GList *l;
	connection *con;

	UNUSED(loop);
	UNUSED(revents);

	while ( NULL != (l = g_queue_peek_head_link(q)) &&
	        (con = (connection*) l->data)->keep_alive_data.timeout <= now ) {
		guint timeout = GPOINTER_TO_INT(CORE_OPTION(CORE_OPTION_MAX_KEEP_ALIVE_IDLE));
		ev_tstamp remaining = timeout - srv->keep_alive_queue_timeout - (now - con->keep_alive_data.timeout);
		if (remaining > 0) {
			ev_timer_set(&con->keep_alive_data.watcher, remaining, 0);
			ev_timer_start(srv->loop, &con->keep_alive_data.watcher);
		} else {
			/* close it */
			con_put(srv, con);
		}
	}

	if (NULL == l) {
		ev_timer_stop(srv->loop, &srv->keep_alive_timer);
	} else {
		srv->keep_alive_timer.repeat = con->keep_alive_data.timeout - now + 1;
		ev_timer_again(srv->loop, &srv->keep_alive_timer);
	}
}

server* server_new() {
	server* srv = g_slice_new0(server);

	srv->magic = LIGHTTPD_SERVER_MAGIC;
	srv->state = SERVER_STARTING;

	srv->connections_active = 0;
	srv->connections = g_array_new(FALSE, TRUE, sizeof(connection*));
	srv->sockets = g_array_new(FALSE, TRUE, sizeof(server_socket*));
	g_queue_init(&srv->closing_sockets);

	srv->plugins = g_hash_table_new(g_str_hash, g_str_equal);
	srv->options = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, server_option_free);
	srv->actions = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, server_action_free);
	srv->setups  = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, server_setup_free);

	srv->plugins_handle_close = g_array_new(FALSE, TRUE, sizeof(plugin*));

	srv->mainaction = NULL;

	srv->exiting = FALSE;
	srv->tmp_str = g_string_sized_new(255);

	srv->last_generated_date_ts = 0;
	srv->ts_date_str = g_string_sized_new(255);

	log_init(srv);

	g_queue_init(&srv->keep_alive_queue);
	my_ev_init(&srv->keep_alive_timer, server_keepalive_cb);
	srv->keep_alive_timer.data = srv;

	return srv;
}

void server_free(server* srv) {
	if (!srv) return;

	srv->exiting = TRUE;
	server_stop(srv);

	{ /* close connections */
		guint i;
		if (srv->connections_active > 0) {
			ERROR(srv, "Server shutdown with unclosed connections: %u", srv->connections_active);
			for (i = srv->connections_active; i-- > 0;) {
				connection *con = g_array_index(srv->connections, connection*, i);
				connection_set_state(srv, con, CON_STATE_ERROR);
				connection_state_machine(srv, con); /* cleanup plugins */
				con_put(srv, con);
			}
		}
		for (i = 0; i < srv->connections->len; i++) {
			connection_free(srv, g_array_index(srv->connections, connection*, i));
		}
		g_array_free(srv->connections, TRUE);
	}

	{
		guint i; for (i = 0; i < srv->sockets->len; i++) {
			server_socket *sock = g_array_index(srv->sockets, server_socket*, i);
			close(sock->watcher.fd);
			g_slice_free(server_socket, sock);
		}
		g_array_free(srv->sockets, TRUE);
	}
	{ /* force closing sockets */
		GList *iter;
		for (iter = g_queue_peek_head_link(&srv->closing_sockets); iter; iter = g_list_next(iter)) {
			server_closing_socket_cb(EV_TIMEOUT, (server_closing_socket*) iter->data);
		}
		g_queue_clear(&srv->closing_sockets);
	}

	g_hash_table_destroy(srv->plugins);
	g_hash_table_destroy(srv->options);
	g_hash_table_destroy(srv->actions);
	g_hash_table_destroy(srv->setups);

	g_array_free(srv->plugins_handle_close, TRUE);

	action_release(srv, srv->mainaction);

	g_string_free(srv->tmp_str, TRUE);
	g_string_free(srv->ts_date_str, TRUE);

	/* free logs */
	g_thread_join(srv->log_thread);
	{
		GHashTableIter iter;
		gpointer k, v;
		g_hash_table_iter_init(&iter, srv->logs);
		while (g_hash_table_iter_next(&iter, &k, &v)) {
			log_free(srv, v);
		}
		g_hash_table_destroy(srv->logs);
	}

	g_mutex_free(srv->log_mutex);
	g_async_queue_unref(srv->log_queue);

	g_queue_clear(&srv->keep_alive_queue);
	ev_timer_stop(srv->loop, &srv->keep_alive_timer);

	g_slice_free(server, srv);
}

gboolean server_loop_init(server *srv) {
	srv->loop = ev_default_loop(srv->loop_flags);

	if (!srv->loop) {
		fatal ("could not initialise libev, bad $LIBEV_FLAGS in environment?");
		return FALSE;
	}

	CATCH_SIGNAL(srv->loop, sigint_cb, INT);
	CATCH_SIGNAL(srv->loop, sigint_cb, TERM);
	CATCH_SIGNAL(srv->loop, sigpipe_cb, PIPE);

	return TRUE;
}

static connection* con_get(server *srv) {
	connection *con;
	if (srv->connections_active >= srv->connections->len) {
		con = connection_new(srv);
		con->idx = srv->connections_active++;
		g_array_append_val(srv->connections, con);
	} else {
		con = g_array_index(srv->connections, connection*, srv->connections_active++);
	}
	return con;
}

void con_put(server *srv, connection *con) {
	connection_reset(srv, con);
	srv->connections_active--;
	if (con->idx != srv->connections_active) {
		/* Swap [con->idx] and [srv->connections_active] */
		connection *tmp;
		assert(con->idx < srv->connections_active); /* con must be an active connection) */
		tmp = g_array_index(srv->connections, connection*, srv->connections_active);
		tmp->idx = con->idx;
		con->idx = srv->connections_active;
		g_array_index(srv->connections, connection*, con->idx) = con;
		g_array_index(srv->connections, connection*, tmp->idx) = tmp;
	}
}

static void server_listen_cb(struct ev_loop *loop, ev_io *w, int revents) {
	server_socket *sock = (server_socket*) w->data;
	server *srv = sock->srv;
	int s;
	sock_addr remote_addr;
	socklen_t l = sizeof(remote_addr);
	UNUSED(loop);
	UNUSED(revents);

	while (-1 != (s = accept(w->fd, (struct sockaddr*) &remote_addr, &l))) {
		connection *con = con_get(srv);
		con->state = CON_STATE_REQUEST_START;
		con->remote_addr = remote_addr;
		ev_io_set(&con->sock.watcher, s, EV_READ);
		ev_io_start(srv->loop, &con->sock.watcher);
	}

#ifdef _WIN32
	errno = WSAGetLastError();
#endif

	switch (errno) {
	case EAGAIN:
#if EWOULDBLOCK != EAGAIN
	case EWOULDBLOCK:
#endif
	case EINTR:
		/* we were stopped _before_ we had a connection */
	case ECONNABORTED: /* this is a FreeBSD thingy */
		/* we were stopped _after_ we had a connection */
		break;

	case EMFILE: /* we are out of FDs */
		/* TODO: server_out_of_fds(srv, NULL); */
		break;
	default:
		ERROR(srv, "accept failed on fd=%d with error: %s", w->fd, g_strerror(errno));
		break;
	}
}

void server_listen(server *srv, int fd) {
	server_socket *sock;

	sock = g_slice_new0(server_socket);
	sock->srv = srv;
	sock->watcher.data = sock;
	fd_init(fd);
	my_ev_init(&sock->watcher, server_listen_cb);
	ev_io_set(&sock->watcher, fd, EV_READ);
	if (srv->state == SERVER_RUNNING) ev_io_start(srv->loop, &sock->watcher);

	g_array_append_val(srv->sockets, sock);
}

void server_start(server *srv) {
	guint i;
	GHashTableIter iter;
	gpointer k, v;
	if (srv->state == SERVER_STOPPING || srv->state == SERVER_RUNNING) return; /* no restart after stop */
	srv->state = SERVER_RUNNING;

	if (!srv->mainaction) {
		ERROR(srv, "%s", "No action handlers defined");
		server_stop(srv);
		return;
	}

	srv->keep_alive_queue_timeout = 5;

	srv->option_count = g_hash_table_size(srv->options);
	srv->option_def_values = g_slice_alloc0(srv->option_count * sizeof(*srv->option_def_values));

	/* set default option values */
	g_hash_table_iter_init(&iter, srv->options);
	while (g_hash_table_iter_next(&iter, &k, &v)) {
		server_option *so = v;
		srv->option_def_values[so->index] = so->default_value;
	}

	plugins_prepare_callbacks(srv);

	for (i = 0; i < srv->sockets->len; i++) {
		server_socket *sock = g_array_index(srv->sockets, server_socket*, i);
		ev_io_start(srv->loop, &sock->watcher);
	}

	srv->started = ev_now(srv->loop);

	log_thread_start(srv);

	ev_loop(srv->loop, 0);
}

void server_stop(server *srv) {
	guint i;
	if (srv->state == SERVER_STOPPING) return;
	srv->state = SERVER_STOPPING;

	for (i = 0; i < srv->sockets->len; i++) {
		server_socket *sock = g_array_index(srv->sockets, server_socket*, i);
		ev_io_stop(srv->loop, &sock->watcher);
	}

	for (i = srv->connections_active; i-- > 0;) {
		connection *con = g_array_index(srv->connections, connection*, i);
		if (con->state == CON_STATE_KEEP_ALIVE)
			con_put(srv, con);
	}
}

void server_exit(server *srv) {
	g_atomic_int_set(&srv->exiting, TRUE);
	server_stop(srv);

	{ /* force closing sockets */
		GList *iter;
		for (iter = g_queue_peek_head_link(&srv->closing_sockets); iter; iter = g_list_next(iter)) {
			server_rem_closing_socket(srv, (server_closing_socket*) iter->data);
		}
	}

	log_thread_wakeup(srv);
}

void joblist_append(server *srv, connection *con) {
	connection_state_machine(srv, con);
}

GString *server_current_timestamp(server *srv) {
	time_t cur_ts = CUR_TS(srv);
	if (cur_ts != srv->last_generated_date_ts) {
		g_string_set_size(srv->ts_date_str, 255);
		strftime(srv->ts_date_str->str, srv->ts_date_str->allocated_len,
				"%a, %d %b %Y %H:%M:%S GMT", gmtime(&(cur_ts)));

		g_string_set_size(srv->ts_date_str, strlen(srv->ts_date_str->str));

		srv->last_generated_date_ts = cur_ts;
	}
	return srv->ts_date_str;
}


#include "base.h"
#include "utils.h"

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

server* server_new() {
	server* srv = g_slice_new0(server);

	srv->magic = LIGHTTPD_SERVER_MAGIC;
	srv->state = SERVER_STARTING;

	srv->loop = ev_default_loop (0);
	if (!srv->loop) {
		fatal ("could not initialise libev, bad $LIBEV_FLAGS in environment?");
	}

	srv->connections_active = 0;
	srv->connections = g_array_new(FALSE, TRUE, sizeof(connection*));
	srv->sockets = g_array_new(FALSE, TRUE, sizeof(server_socket*));

	srv->plugins = g_hash_table_new(g_str_hash, g_str_equal);
	srv->options = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, server_option_free);
	srv->actions = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, server_action_free);
	srv->setups  = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, server_setup_free);

	srv->mainaction = NULL;

	srv->exiting = FALSE;
	srv->tmp_str = g_string_sized_new(255);

	srv->cur_ts = time(0);
	srv->last_generated_date_ts = 0;
	srv->ts_date_str = g_string_sized_new(255);

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

	g_hash_table_destroy(srv->plugins);
	g_hash_table_destroy(srv->options);
	g_hash_table_destroy(srv->actions);
	g_hash_table_destroy(srv->setups);

	action_release(srv, srv->mainaction);

	g_string_free(srv->tmp_str, TRUE);
	g_string_free(srv->ts_date_str, TRUE);

	/* free logs */
	GHashTableIter iter;
	gpointer k, v;
	g_hash_table_iter_init(&iter, srv->logs);
	while (g_hash_table_iter_next(&iter, &k, &v)) {
		log_free(srv, v);
	}
	g_hash_table_destroy(srv->logs);

	g_mutex_free(srv->log_mutex);
	g_async_queue_unref(srv->log_queue);

	g_slice_free(server, srv);
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
		ERROR(srv, "accept failed on fd=%d with error: (%d) %s", w->fd, errno, strerror(errno));
		break;
	}
}

void server_listen(server *srv, int fd) {
	server_socket *sock;

	sock = g_slice_new0(server_socket);
	sock->srv = srv;
	sock->watcher.data = sock;
	fd_init(fd);
	ev_io_init(&sock->watcher, server_listen_cb, fd, EV_READ);
	if (srv->state == SERVER_RUNNING) ev_io_start(srv->loop, &sock->watcher);

	g_array_append_val(srv->sockets, sock);
}


static void sigint_cb(struct ev_loop *loop, struct ev_signal *w, int revents) {
	UNUSED(w); UNUSED(revents);
	ev_unloop (loop, EVUNLOOP_ALL);
}

static void sigpipe_cb(struct ev_loop *loop, struct ev_signal *w, int revents) {
	/* ignore */
	UNUSED(loop); UNUSED(w); UNUSED(revents);
}

static struct ev_signal
	sig_w_INT,
	sig_w_TERM,
	sig_w_PIPE;

#define CATCH_SIGNAL(loop, cb, n) do {\
	ev_signal_init(&sig_w_##n, cb, SIG##n); \
	ev_signal_start(loop, &sig_w_##n); \
} while (0)

void server_start(server *srv) {
	guint i;
	if (srv->state == SERVER_STOPPING || srv->state == SERVER_RUNNING) return; /* no restart after stop */
	srv->state = SERVER_RUNNING;

	if (!srv->mainaction) {
		ERROR(srv, "%s", "No action handlers defined");
		server_stop(srv);
		return;
	}

	for (i = 0; i < srv->sockets->len; i++) {
		server_socket *sock = g_array_index(srv->sockets, server_socket*, i);
		ev_io_start(srv->loop, &sock->watcher);
	}

	CATCH_SIGNAL(srv->loop, sigint_cb, INT);
	CATCH_SIGNAL(srv->loop, sigint_cb, TERM);
	CATCH_SIGNAL(srv->loop, sigpipe_cb, PIPE);

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
}

void joblist_append(server *srv, connection *con) {
	connection_state_machine(srv, con);
}

GString *server_current_timestamp(server *srv) {
	srv->cur_ts = time(0); /* TODO: update cur_ts somewhere else */
	if (srv->cur_ts != srv->last_generated_date_ts) {
		g_string_set_size(srv->ts_date_str, 255);
		strftime(srv->ts_date_str->str, srv->ts_date_str->allocated_len,
				"%a, %d %b %Y %H:%M:%S GMT", gmtime(&(srv->cur_ts)));

		g_string_set_size(srv->ts_date_str, strlen(srv->ts_date_str->str));

		srv->last_generated_date_ts = srv->cur_ts;
	}
	return srv->ts_date_str;
}

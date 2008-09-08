
#include "base.h"
#include "utils.h"
#include "plugin_core.h"

void con_put(connection *con);

static void server_option_free(gpointer _so) {
	g_slice_free(server_option, _so);
}

static void server_action_free(gpointer _sa) {
	g_slice_free(server_action, _sa);
}

static void server_setup_free(gpointer _ss) {
	g_slice_free(server_setup, _ss);
}

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
	ev_init(&srv->sig_w_##n, cb); \
	ev_signal_set(&srv->sig_w_##n, SIG##n); \
	ev_signal_start(loop, &srv->sig_w_##n); \
	srv->sig_w_##n.data = srv; \
	ev_unref(loop); /* Signal watchers shouldn't keep loop alive */ \
} while (0)

server* server_new() {
	server* srv = g_slice_new0(server);

	srv->magic = LIGHTTPD_SERVER_MAGIC;
	srv->state = SERVER_STARTING;

	srv->connections_active = 0;
	srv->connections = g_array_new(FALSE, TRUE, sizeof(connection*));
	srv->sockets = g_array_new(FALSE, TRUE, sizeof(server_socket*));

	srv->plugins = g_hash_table_new(g_str_hash, g_str_equal);
	srv->options = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, server_option_free);
	srv->actions = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, server_action_free);
	srv->setups  = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, server_setup_free);

	srv->plugins_handle_close = g_array_new(FALSE, TRUE, sizeof(plugin*));

	srv->mainaction = NULL;

	srv->exiting = FALSE;

	log_init(srv);

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
				connection_set_state(con, CON_STATE_ERROR);
				connection_state_machine(con); /* cleanup plugins */
			}
		}
		for (i = 0; i < srv->connections->len; i++) {
			connection_free(g_array_index(srv->connections, connection*, i));
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

	g_hash_table_destroy(srv->plugins);
	g_hash_table_destroy(srv->options);
	g_hash_table_destroy(srv->actions);
	g_hash_table_destroy(srv->setups);

	g_array_free(srv->plugins_handle_close, TRUE);

	action_release(srv, srv->mainaction);

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

	g_slice_free(server, srv);
}

gboolean server_loop_init(server *srv) {
	struct ev_loop *loop = ev_default_loop(srv->loop_flags);

	if (!loop) {
		fatal ("could not initialise libev, bad $LIBEV_FLAGS in environment?");
		return FALSE;
	}

	CATCH_SIGNAL(loop, sigint_cb, INT);
	CATCH_SIGNAL(loop, sigint_cb, TERM);
	CATCH_SIGNAL(loop, sigpipe_cb, PIPE);

	srv->main_worker = worker_new(srv, loop);

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

void con_put(connection *con) {
	server *srv = con->srv;

	connection_reset(con);
	con->wrk = NULL;
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
		con->wrk = srv->main_worker; /* TODO: balance workers; push con in a queue for the worker */
		con->state = CON_STATE_REQUEST_START;
		con->remote_addr = remote_addr;
		ev_io_set(&con->sock_watcher, s, EV_READ);
		ev_io_start(con->wrk->loop, &con->sock_watcher);
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
	ev_init(&sock->watcher, server_listen_cb);
	ev_io_set(&sock->watcher, fd, EV_READ);
	if (srv->state == SERVER_RUNNING) ev_io_start(srv->main_worker->loop, &sock->watcher);

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
		if (so->default_value)
			srv->option_def_values[so->index] = so->default_value(srv, so->p, so->index);
	}

	plugins_prepare_callbacks(srv);

	for (i = 0; i < srv->sockets->len; i++) {
		server_socket *sock = g_array_index(srv->sockets, server_socket*, i);
		ev_io_start(srv->main_worker->loop, &sock->watcher);
	}

	srv->started = ev_now(srv->main_worker->loop);

	log_thread_start(srv);

	worker_run(srv->main_worker);
}

void server_stop(server *srv) {
	guint i;
	if (srv->state == SERVER_STOPPING) return;
	srv->state = SERVER_STOPPING;

	for (i = 0; i < srv->sockets->len; i++) {
		server_socket *sock = g_array_index(srv->sockets, server_socket*, i);
		ev_io_stop(srv->main_worker->loop, &sock->watcher);
	}

	for (i = srv->connections_active; i-- > 0;) {
		connection *con = g_array_index(srv->connections, connection*, i);
		if (con->state == CON_STATE_KEEP_ALIVE)
			con_put(con);
	}
}

void server_exit(server *srv) {
	g_atomic_int_set(&srv->exiting, TRUE);
	server_stop(srv);

	log_thread_wakeup(srv);
}

void joblist_append(connection *con) {
	connection_state_machine(con);
}

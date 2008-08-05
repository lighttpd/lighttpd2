
#include "base.h"
#include "utils.h"

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

	return srv;
}

void server_free(server* srv) {
	if (!srv) return;
	/* TODO */

	g_hash_table_destroy(srv->options);
	g_hash_table_destroy(srv->actions);
	g_hash_table_destroy(srv->setups);
	g_hash_table_destroy(srv->plugins);

	action_release(srv, srv->mainaction);

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
		g_array_set_size(srv->connections, srv->connections->len + 10);
		g_array_index(srv->connections, connection*, con->idx) = con;
	} else {
		con = g_array_index(srv->connections, connection*, srv->connections_active++);
	}
	return con;
}

static void con_put(server *srv, connection *con) {
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

void server_start(server *srv) {
	guint i;
	if (srv->state == SERVER_STOPPING || srv->state == SERVER_RUNNING) return; /* no restart after stop */
	srv->state = SERVER_RUNNING;

	for (i = 0; i < srv->sockets->len; i++) {
		server_socket *sock = g_array_index(srv->sockets, server_socket*, i);
		ev_io_start(srv->loop, &sock->watcher);
	}
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
	/* TODO */
}

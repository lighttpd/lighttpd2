
#include "base.h"
#include "utils.h"
#include "plugin_core.h"

static void server_value_free(gpointer _so) {
	g_slice_free(server_option, _so);
}

static void server_action_free(gpointer _sa) {
	g_slice_free(server_action, _sa);
}

static void server_setup_free(gpointer _ss) {
	g_slice_free(server_setup, _ss);
}

#define CATCH_SIGNAL(loop, cb, n) do {\
	ev_init(&srv->sig_w_##n, cb); \
	ev_signal_set(&srv->sig_w_##n, SIG##n); \
	ev_signal_start(loop, &srv->sig_w_##n); \
	srv->sig_w_##n.data = srv; \
	ev_unref(loop); /* Signal watchers shouldn't keep loop alive */ \
} while (0)

#define UNCATCH_SIGNAL(loop, n) do {\
	ev_ref(loop); \
	ev_signal_stop(loop, &srv->sig_w_##n); \
} while (0)

static void sigint_cb(struct ev_loop *loop, struct ev_signal *w, int revents) {
	server *srv = (server*) w->data;
	UNUSED(revents);

	if (g_atomic_int_get(&srv->state) != SERVER_STOPPING) {
		INFO(srv, "Got signal, shutdown");
		server_stop(srv);
	} else {
		INFO(srv, "Got second signal, force shutdown");

		/* reset default behaviour which will kill us the third time */
		UNCATCH_SIGNAL(loop, INT);
		UNCATCH_SIGNAL(loop, TERM);
		UNCATCH_SIGNAL(loop, PIPE);
	}
}

static void sigpipe_cb(struct ev_loop *loop, struct ev_signal *w, int revents) {
	/* ignore */
	UNUSED(loop); UNUSED(w); UNUSED(revents);
}

server* server_new() {
	server* srv = g_slice_new0(server);

	srv->magic = LIGHTTPD_SERVER_MAGIC;
	srv->state = SERVER_STARTING;

	srv->workers = g_array_new(FALSE, TRUE, sizeof(worker*));

	srv->sockets = g_array_new(FALSE, TRUE, sizeof(server_socket*));

	srv->plugins = g_hash_table_new(g_str_hash, g_str_equal);
	srv->options = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, server_value_free);
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

	server_stop(srv);
	g_atomic_int_set(&srv->exiting, TRUE);

	/* join all workers */
	{
		guint i;
		for (i = 1; i < srv->worker_count; i++) {
			worker *wrk;
			wrk = g_array_index(srv->workers, worker*, i);
			worker_exit(srv->main_worker, wrk);
			g_thread_join(wrk->thread);
		}
	}

	/* free all workers */
	{
		guint i;
		for (i = 0; i < srv->worker_count; i++) {
			worker *wrk;
			struct ev_loop *loop;
			wrk = g_array_index(srv->workers, worker*, i);
			loop = wrk->loop;
			worker_free(wrk);
			if (i == 0) {
				ev_default_destroy();
			} else {
				ev_loop_destroy(loop);
			}
		}
		g_array_free(srv->workers, TRUE);
	}

	{
		guint i; for (i = 0; i < srv->sockets->len; i++) {
			server_socket *sock = g_array_index(srv->sockets, server_socket*, i);
			close(sock->watcher.fd);
			g_slice_free(server_socket, sock);
		}
		g_array_free(srv->sockets, TRUE);
	}

	action_release(srv, srv->mainaction);

	server_plugins_free(srv);
	g_array_free(srv->plugins_handle_close, TRUE); /* TODO: */

	if (srv->option_def_values)
		g_slice_free1(srv->option_count * sizeof(*srv->option_def_values), srv->option_def_values);

	/* free logs */
	g_thread_join(srv->logs.thread);
	{
		GHashTableIter iter;
		gpointer k, v;
		g_hash_table_iter_init(&iter, srv->logs.targets);
		while (g_hash_table_iter_next(&iter, &k, &v)) {
			log_free(srv, v);
		}
		g_hash_table_destroy(srv->logs.targets);
	}

	g_mutex_free(srv->logs.mutex);
	g_async_queue_unref(srv->logs.queue);

	g_slice_free(server, srv);
}

static gpointer server_worker_cb(gpointer data) {
	worker *wrk = (worker*) data;
	worker_run(wrk);
	return NULL;
}

gboolean server_loop_init(server *srv) {
	guint i;
	struct ev_loop *loop = ev_default_loop(srv->loop_flags);

	if (!loop) {
		fatal ("could not initialise libev, bad $LIBEV_FLAGS in environment?");
		return FALSE;
	}

	CATCH_SIGNAL(loop, sigint_cb, INT);
	CATCH_SIGNAL(loop, sigint_cb, TERM);
	CATCH_SIGNAL(loop, sigpipe_cb, PIPE);

	if (srv->worker_count < 1) srv->worker_count = 1;
	g_array_set_size(srv->workers, srv->worker_count);
	srv->main_worker = g_array_index(srv->workers, worker*, 0) = worker_new(srv, loop);
	srv->main_worker->ndx = 0;
	for (i = 1; i < srv->worker_count; i++) {
		GError *error = NULL;
		worker *wrk;
		if (NULL == (loop = ev_loop_new(srv->loop_flags))) {
			fatal ("could not create extra libev loops");
			return FALSE;
		}
		wrk = g_array_index(srv->workers, worker*, i) = worker_new(srv, loop);
		wrk->ndx = i;
		if (NULL == (wrk->thread = g_thread_create(server_worker_cb, wrk, TRUE, &error))) {
			g_error ( "g_thread_create failed: %s", error->message );
			return FALSE;
		}
	}

	return TRUE;
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
		worker *wrk = srv->main_worker;
		guint i, min_load = g_atomic_int_get(&wrk->connection_load), sel = 0;

		for (i = 1; i < srv->worker_count; i++) {
			worker *wt = g_array_index(srv->workers, worker*, i);
			guint load = g_atomic_int_get(&wt->connection_load);
			if (load < min_load) {
				wrk = wt;
				min_load = load;
				sel = i;
			}
		}

		g_atomic_int_inc((gint*) &wrk->connection_load);
		/* TRACE(srv, "selected worker %u with load %u", sel, min_load); */
		worker_new_con(srv->main_worker, wrk, &remote_addr, s);
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
	if (g_atomic_int_get(&srv->state) == SERVER_RUNNING) ev_io_start(srv->main_worker->loop, &sock->watcher);

	g_array_append_val(srv->sockets, sock);
}

void server_start(server *srv) {
	guint i;
	GHashTableIter iter;
	gpointer k, v;
	server_state srvstate = g_atomic_int_get(&srv->state);
	if (srvstate == SERVER_STOPPING || srvstate == SERVER_RUNNING) return; /* no restart after stop */
	g_atomic_int_set(&srv->state, SERVER_RUNNING);

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
			srv->option_def_values[so->index].ptr = so->default_value(srv, so->p, so->index);
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

	if (g_atomic_int_get(&srv->state) == SERVER_STOPPING) return;
	g_atomic_int_set(&srv->state, SERVER_STOPPING);

	for (i = 0; i < srv->sockets->len; i++) {
		server_socket *sock = g_array_index(srv->sockets, server_socket*, i);
		ev_io_stop(srv->main_worker->loop, &sock->watcher);
	}

	/* stop all workers */
	for (i = 0; i < srv->worker_count; i++) {
		worker *wrk;
		wrk = g_array_index(srv->workers, worker*, i);
		worker_stop(srv->main_worker, wrk);
	}

	log_thread_wakeup(srv);
}

void server_exit(server *srv) {
	server_stop(srv);

	g_atomic_int_set(&srv->exiting, TRUE);

	/* exit all workers */
	{
		guint i;
		for (i = 0; i < srv->worker_count; i++) {
			worker *wrk;
			wrk = g_array_index(srv->workers, worker*, i);
			worker_exit(srv->main_worker, wrk);
		}
	}
}

void joblist_append(connection *con) {
	connection_state_machine(con);
}

/* cache timestamp */
GString *server_current_timestamp() {
	static GStaticPrivate last_ts_key = G_STATIC_PRIVATE_INIT;
	static GStaticPrivate ts_str_key = G_STATIC_PRIVATE_INIT;

	time_t *last_ts = g_static_private_get(&last_ts_key);
	GString *ts_str = g_static_private_get(&ts_str_key);

	time_t cur_ts = time(NULL);

	if (last_ts == NULL) {
		last_ts = g_new0(time_t, 1);
		g_static_private_set(&last_ts_key, last_ts, g_free);
	}
	if (ts_str == NULL) {
		ts_str = g_string_sized_new(255);
		g_static_private_set(&ts_str_key, ts_str, (GDestroyNotify)string_destroy_notify);
	}

	if (cur_ts != *last_ts) {
		gsize s;

		g_string_set_size(ts_str, 255);
		s = strftime(ts_str->str, ts_str->allocated_len,
				"%a, %d %b %Y %H:%M:%S GMT", gmtime(&(cur_ts)));
		g_string_set_size(ts_str, s);
		*last_ts = cur_ts;
	}

	return ts_str;
}

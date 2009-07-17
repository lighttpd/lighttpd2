
#include <lighttpd/base.h>
#include <lighttpd/plugin_core.h>

static void li_server_listen_cb(struct ev_loop *loop, ev_io *w, int revents);

static liServerSocket* server_socket_new(int fd) {
	liServerSocket *sock = g_slice_new0(liServerSocket);

	sock->refcount = 1;
	sock->watcher.data = sock;
	sock->local_addr = li_sockaddr_local_from_socket(fd);
	sock->local_addr_str = g_string_sized_new(0);
	li_sockaddr_to_string(sock->local_addr, sock->local_addr_str, FALSE);
	li_fd_init(fd);
	ev_init(&sock->watcher, li_server_listen_cb);
	ev_io_set(&sock->watcher, fd, EV_READ);
	return sock;
}

void li_server_socket_release(liServerSocket* sock) {
	if (!sock) return;
	assert(g_atomic_int_get(&sock->refcount) > 0);
	if (g_atomic_int_dec_and_test(&sock->refcount)) {
		li_sockaddr_clear(&sock->local_addr);
		g_string_free(sock->local_addr_str, TRUE);
		g_slice_free(liServerSocket, sock);
	}
}

void li_server_socket_acquire(liServerSocket* sock) {
	assert(g_atomic_int_get(&sock->refcount) > 0);
	g_atomic_int_inc(&sock->refcount);
}

static void server_value_free(gpointer _so) {
	g_slice_free(liServerOption, _so);
}

static void server_action_free(gpointer _sa) {
	g_slice_free(liServerAction, _sa);
}

static void server_setup_free(gpointer _ss) {
	g_slice_free(liServerSetup, _ss);
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
	liServer *srv = (liServer*) w->data;
	UNUSED(revents);

	if (g_atomic_int_get(&srv->state) != LI_SERVER_STOPPING) {
		INFO(srv, "%s", "Got signal, shutdown");
		li_server_stop(srv);
	} else {
		INFO(srv, "%s", "Got second signal, force shutdown");

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

liServer* li_server_new(const gchar *module_dir) {
	liServer* srv = g_slice_new0(liServer);

	srv->magic = LIGHTTPD_SERVER_MAGIC;
	srv->state = LI_SERVER_STARTING;

	srv->workers = g_array_new(FALSE, TRUE, sizeof(liWorker*));
	srv->worker_count = 0;

	srv->sockets = g_ptr_array_new();

	srv->modules = li_modules_new(srv, module_dir);

	srv->plugins = g_hash_table_new(g_str_hash, g_str_equal);
	srv->options = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, server_value_free);
	srv->actions = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, server_action_free);
	srv->setups  = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, server_setup_free);

	srv->li_plugins_handle_close = g_array_new(FALSE, TRUE, sizeof(liPlugin*));
	srv->li_plugins_handle_vrclose = g_array_new(FALSE, TRUE, sizeof(liPlugin*));
	srv->option_def_values = g_array_new(FALSE, TRUE, sizeof(liOptionValue));

	srv->mainaction = NULL;

	srv->exiting = FALSE;

	srv->ts_formats = g_array_new(FALSE, TRUE, sizeof(GString*));
	/* error log ts format */
	li_server_ts_format_add(srv, g_string_new("%a, %d %b %Y %H:%M:%S %Z"));
	/* http header ts format */
	li_server_ts_format_add(srv, g_string_new("%a, %d %b %Y %H:%M:%S GMT"));

	srv->throttle_pools = g_array_new(FALSE, TRUE, sizeof(liThrottlePool*));

	log_init(srv);

	srv->io_timeout = 300; /* default I/O timeout */

	return srv;
}

void li_server_free(liServer* srv) {
	if (!srv) return;

	li_server_stop(srv);
	g_atomic_int_set(&srv->exiting, TRUE);

	/* join all workers */
	{
		guint i;
		for (i = 1; i < srv->workers->len; i++) {
			liWorker *wrk;
			wrk = g_array_index(srv->workers, liWorker*, i);
			li_worker_exit(srv->main_worker, wrk);
			g_thread_join(wrk->thread);
		}
	}

	li_action_release(srv, srv->mainaction);

	/* free throttle pools */
	{
		guint i;
		for (i = 0; i < srv->throttle_pools->len; i++) {
			throttle_pool_free(srv, g_array_index(srv->throttle_pools, liThrottlePool*, i));
		}
		g_array_free(srv->throttle_pools, TRUE);
	}

	/* free all workers */
	{
		guint i;
		for (i = 0; i < srv->workers->len; i++) {
			liWorker *wrk;
			struct ev_loop *loop;
			wrk = g_array_index(srv->workers, liWorker*, i);
			loop = wrk->loop;
			li_worker_free(wrk);
			if (i == 0) {
				ev_default_destroy();
			} else {
				ev_loop_destroy(loop);
			}
		}
		g_array_free(srv->workers, TRUE);
	}

	/* release modules */
	li_modules_free(srv->modules);

	li_plugin_free(srv, srv->core_plugin);

	log_cleanup(srv);

	{
		guint i; for (i = 0; i < srv->sockets->len; i++) {
			liServerSocket *sock = g_ptr_array_index(srv->sockets, i);
			close(sock->watcher.fd);
			li_server_socket_release(sock);
		}
		g_ptr_array_free(srv->sockets, TRUE);
	}

	{
		guint i;
		for (i = 0; i < srv->ts_formats->len; i++)
			g_string_free(g_array_index(srv->ts_formats, GString*, i), TRUE);
		g_array_free(srv->ts_formats, TRUE);
	}

	g_array_free(srv->option_def_values, TRUE);
	li_server_plugins_free(srv);
	g_array_free(srv->li_plugins_handle_close, TRUE);
	g_array_free(srv->li_plugins_handle_vrclose, TRUE);

	if (srv->started_str)
		g_string_free(srv->started_str, TRUE);

	g_slice_free(liServer, srv);
}

static gpointer server_worker_cb(gpointer data) {
	liWorker *wrk = (liWorker*) data;
	li_worker_run(wrk);
	return NULL;
}

gboolean li_server_loop_init(liServer *srv) {
	srv->loop = ev_default_loop(srv->loop_flags);

	if (!srv->loop) {
		li_fatal ("could not initialise libev, bad $LIBEV_FLAGS in environment?");
		return FALSE;
	}

	return TRUE;
}

gboolean li_server_worker_init(liServer *srv) {
	struct ev_loop *loop = srv->loop;
	guint i;

	CATCH_SIGNAL(loop, sigint_cb, INT);
	CATCH_SIGNAL(loop, sigint_cb, TERM);
	CATCH_SIGNAL(loop, sigpipe_cb, PIPE);

	if (srv->worker_count < 1) srv->worker_count = 1;
	g_array_set_size(srv->workers, srv->worker_count);
	srv->main_worker = g_array_index(srv->workers, liWorker*, 0) = li_worker_new(srv, loop);
	srv->main_worker->ndx = 0;
	for (i = 1; i < srv->worker_count; i++) {
		GError *error = NULL;
		liWorker *wrk;
		if (NULL == (loop = ev_loop_new(srv->loop_flags))) {
			li_fatal ("could not create extra libev loops");
			return FALSE;
		}
		wrk = g_array_index(srv->workers, liWorker*, i) = li_worker_new(srv, loop);
		wrk->ndx = i;
		if (NULL == (wrk->thread = g_thread_create(server_worker_cb, wrk, TRUE, &error))) {
			g_error ( "g_thread_create failed: %s", error->message );
			return FALSE;
		}
	}

	return TRUE;
}

static void li_server_listen_cb(struct ev_loop *loop, ev_io *w, int revents) {
	liServerSocket *sock = (liServerSocket*) w->data;
	liServer *srv = sock->srv;
	int s;
	liSocketAddress remote_addr;
	struct sockaddr sa;
	socklen_t l = sizeof(sa);
	UNUSED(loop);
	UNUSED(revents);

	while (-1 != (s = accept(w->fd, &sa, &l))) {
		liWorker *wrk = srv->main_worker;
		guint i, min_load = g_atomic_int_get(&wrk->connection_load);

		if (l <= sizeof(sa)) {
			remote_addr.addr = g_slice_alloc(l);
			remote_addr.len = l;
			memcpy(remote_addr.addr, &sa, l);
		} else {
			remote_addr = li_sockaddr_remote_from_socket(s);
		}
		l = sizeof(sa); /* reset l */

		li_fd_init(s);

		for (i = 1; i < srv->worker_count; i++) {
			liWorker *wt = g_array_index(srv->workers, liWorker*, i);
			guint load = g_atomic_int_get(&wt->connection_load);
			if (load < min_load) {
				wrk = wt;
				min_load = load;
			}
		}

		g_atomic_int_inc((gint*) &wrk->connection_load);
		li_server_socket_acquire(sock);
		li_worker_new_con(srv->main_worker, wrk, remote_addr, s, sock);
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
		li_server_out_of_fds(srv);
		/* TODO: disable accept callbacks? */
		break;
	default:
		ERROR(srv, "accept failed on fd=%d with error: %s", w->fd, g_strerror(errno));
		break;
	}
}

void li_server_listen(liServer *srv, int fd) {
	liServerSocket *sock = server_socket_new(fd);

	sock->srv = srv;
	g_ptr_array_add(srv->sockets, sock);

	if (g_atomic_int_get(&srv->state) == LI_SERVER_RUNNING) ev_io_start(srv->main_worker->loop, &sock->watcher);
}

void li_server_start(liServer *srv) {
	guint i;
	liServerState srvstate = g_atomic_int_get(&srv->state);
	if (srvstate == LI_SERVER_STOPPING || srvstate == LI_SERVER_RUNNING) return; /* no restart after stop */
	g_atomic_int_set(&srv->state, LI_SERVER_RUNNING);

	if (!srv->mainaction) {
		ERROR(srv, "%s", "No action handlers defined");
		li_server_stop(srv);
		return;
	}

	srv->keep_alive_queue_timeout = 5;

	li_plugins_prepare_callbacks(srv);

	for (i = 0; i < srv->sockets->len; i++) {
		liServerSocket *sock = g_ptr_array_index(srv->sockets, i);
		ev_io_start(srv->main_worker->loop, &sock->watcher);
	}

	srv->started = ev_now(srv->main_worker->loop);
	{
		GString *str = li_worker_current_timestamp(srv->main_worker, LI_LOCALTIME, LI_TS_FORMAT_DEFAULT);
		srv->started = ev_now(srv->main_worker->loop);
		srv->started_str = g_string_new_len(GSTR_LEN(str));
	}

	log_thread_start(srv);

	li_worker_run(srv->main_worker);
}

void li_server_stop(liServer *srv) {
	guint i;

	if (g_atomic_int_get(&srv->state) == LI_SERVER_STOPPING) return;
	g_atomic_int_set(&srv->state, LI_SERVER_STOPPING);

	if (srv->main_worker) {
		for (i = 0; i < srv->sockets->len; i++) {
			liServerSocket *sock = g_ptr_array_index(srv->sockets, i);
			ev_io_stop(srv->main_worker->loop, &sock->watcher);
		}

		/* stop all workers */
		for (i = 0; i < srv->worker_count; i++) {
			liWorker *wrk;
			wrk = g_array_index(srv->workers, liWorker*, i);
			li_worker_stop(srv->main_worker, wrk);
		}
	}

	log_thread_wakeup(srv);
}

void li_server_exit(liServer *srv) {
	li_server_stop(srv);

	g_atomic_int_set(&srv->exiting, TRUE);

	/* exit all workers */
	{
		guint i;
		for (i = 0; i < srv->worker_count; i++) {
			liWorker *wrk;
			wrk = g_array_index(srv->workers, liWorker*, i);
			li_worker_exit(srv->main_worker, wrk);
		}
	}
}

/* cache timestamp */
GString *li_server_current_timestamp() {
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
		g_static_private_set(&ts_str_key, ts_str, (GDestroyNotify)li_string_destroy_notify);
	}

	if (cur_ts != *last_ts) {
		gsize s;
		struct tm tm;

		g_string_set_size(ts_str, 255);
#ifdef HAVE_GMTIME_R
		s = strftime(ts_str->str, ts_str->allocated_len,
				"%a, %d %b %Y %H:%M:%S GMT", gmtime_r(&cur_ts, &tm));
#else
		s = strftime(ts_str->str, ts_str->allocated_len,
				"%a, %d %b %Y %H:%M:%S GMT", gmtime(&cur_ts));
#endif
		g_string_set_size(ts_str, s);
		*last_ts = cur_ts;
	}

	return ts_str;
}

void li_server_out_of_fds(liServer *srv) {
	ERROR(srv, "%s", "Too many open files. Either raise your fd limit or use a lower connection limit.");
}

guint li_server_ts_format_add(liServer *srv, GString* format) {
	/* check if not already registered */
	guint i;
	for (i = 0; i < srv->ts_formats->len; i++) {
		if (g_string_equal(g_array_index(srv->ts_formats, GString*, i), format))
			return i;
	}

	g_array_append_val(srv->ts_formats, format);
	return i;
}

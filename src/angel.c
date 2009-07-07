
#include <lighttpd/base.h>
#include <lighttpd/angel.h>

static void angel_call_cb(angel_connection *acon,
		const gchar *mod, gsize mod_len, const gchar *action, gsize action_len,
		gint32 id, GString *data) {
	server *srv = acon->data;
	ERROR(srv, "received message for %s:%s, not implemented yet", mod, action);
	if (-1 != id) angel_send_result(acon, id, g_string_new_len(CONST_STR_LEN("not implemented yet")), NULL, NULL, NULL);
}

static void angel_close_cb(angel_connection *acon, GError *err) {
	server *srv = acon->data;
	ERROR(srv, "fatal: angel connection close: %s", err ? err->message : g_strerror(errno));
	if (err) g_error_free(err);
	exit(1);
}

void angel_setup(server *srv) {
	srv->acon = angel_connection_new(srv->loop, 0, srv, angel_call_cb, angel_close_cb);
}

static void angel_listen_cb(angel_call *acall, gpointer ctx, gboolean timeout, GString *error, GString *data, GArray *fds) {
	server *srv = ctx;
	guint i;
	UNUSED(data);

	angel_call_free(acall);

	ERROR(srv, "%s", "listen_cb");

	if (timeout) {
		ERROR(srv, "listen failed: %s", "time out");
		return;
	}

	if (error->len > 0) {
		ERROR(srv, "listen failed: %s", error->str);
		/* TODO: exit? */
		return;
	}

	if (fds && fds->len > 0) {
		for (i = 0; i < fds->len; i++) {
			INFO(srv, "listening on fd %i", g_array_index(fds, int, i));
			server_listen(srv, g_array_index(fds, int, i));
		}
		g_array_set_size(fds, 0);
	} else {
		ERROR(srv, "listen failed: %s", "received no filedescriptors");
	}
}

/* listen to a socket */
void angel_listen(server *srv, GString *str) {
	if (srv->acon) {
		angel_call *acall = angel_call_new(angel_listen_cb, 3.0);
		GError *err = NULL;

		acall->context = srv;
		if (!angel_send_call(srv->acon, CONST_STR_LEN("core"), CONST_STR_LEN("listen"), acall, g_string_new_len(GSTR_LEN(str)), &err)) {
			ERROR(srv, "couldn't send call: %s", err->message);
			g_error_free(err);
		}
	} else {
		int fd = angel_fake_listen(srv, str);
		if (-1 == fd) {
			ERROR(srv, "listen('%s') failed", str->str);
			/* TODO: exit? */
		} else {
			server_listen(srv, fd);
		}
	}
}

/* send log messages while startup to angel */
void angel_log(server *srv, GString *str) {
	angel_fake_log(srv, str);
}

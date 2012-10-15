
#include <lighttpd/base.h>
#include <lighttpd/angel.h>

static void angel_call_cb(liAngelConnection *acon,
		const gchar *mod, gsize mod_len, const gchar *action, gsize action_len,
		gint32 id, GString *data) {
	liServer *srv = acon->data;
	liPlugin *p;
	const liPluginAngel *acb;
	UNUSED(action_len);
	UNUSED(mod_len);

	if (NULL == (p = g_hash_table_lookup(srv->plugins, mod))) goto not_found;
	if (NULL == p->angelcbs) goto not_found;

	for (acb = p->angelcbs; acb->name; acb++) {
		if (0 == strcmp(acb->name, action)) break;
	}
	if (!acb->name) goto not_found;

	acb->angel_cb(srv, p, id, data);

	return;

not_found:
	ERROR(srv, "received message for %s:%s, couldn't find receiver", mod, action);
	if (-1 != id) li_angel_send_result(acon, id, g_string_new_len(CONST_STR_LEN("receiver not found")), NULL, NULL, NULL);
}

static void angel_close_cb(liAngelConnection *acon, GError *err) {
	liServer *srv = acon->data;
	ERROR(srv, "li_fatal: angel connection close: %s", err ? err->message : g_strerror(errno));
	if (err) g_error_free(err);
	abort();
}

void li_angel_setup(liServer *srv) {
	srv->acon = li_angel_connection_new(srv->loop, 0, srv, angel_call_cb, angel_close_cb);
	srv->dest_state = LI_SERVER_SUSPENDED;
}

typedef struct angel_listen_cb_ctx angel_listen_cb_ctx;
struct angel_listen_cb_ctx {
	liServer *srv;
	liAngelListenCB cb;
	gpointer data;
};

static void li_angel_listen_cb(liAngelCall *acall, gpointer pctx, gboolean timeout, GString *error, GString *data, GArray *fds) {
	angel_listen_cb_ctx ctx = * (angel_listen_cb_ctx*) pctx;
	liServer *srv = ctx.srv;
	guint i;
	UNUSED(data);

	li_angel_call_free(acall);
	g_slice_free(angel_listen_cb_ctx, pctx);

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
			int fd = g_array_index(fds, int, i);
			DEBUG(srv, "listening on fd %i", fd);
			if (ctx.cb) {
				ctx.cb(srv, fd, ctx.data);
			} else {
				li_server_listen(srv, fd);
			}
		}
		g_array_set_size(fds, 0);
	} else {
		ERROR(srv, "listen failed: %s", "received no filedescriptors");
	}
}

/* listen to a socket */
void li_angel_listen(liServer *srv, GString *str, liAngelListenCB cb, gpointer data) {
	if (srv->acon) {
		liAngelCall *acall = li_angel_call_new(li_angel_listen_cb, 20.0);
		angel_listen_cb_ctx *ctx = g_slice_new0(angel_listen_cb_ctx);
		GError *err = NULL;

		ctx->srv = srv;
		ctx->cb = cb;
		ctx->data = data;
		acall->context = ctx;
		if (!li_angel_send_call(srv->acon, CONST_STR_LEN("core"), CONST_STR_LEN("listen"), acall, g_string_new_len(GSTR_LEN(str)), &err)) {
			ERROR(srv, "couldn't send call: %s", err->message);
			g_error_free(err);
		}
	} else {
		int fd = li_angel_fake_listen(srv, str);
		if (-1 == fd) {
			ERROR(srv, "listen('%s') failed", str->str);
			/* TODO: exit? */
		} else {
			if (cb) {
				cb(srv, fd, data);
			} else {
				li_server_listen(srv, fd);
			}
		}
	}
}

/* send log messages while startup to angel */
void li_angel_log(liServer *srv, GString *str) {
	li_angel_fake_log(srv, str);
}

typedef struct angel_log_cb_ctx angel_log_cb_ctx;
struct angel_log_cb_ctx {
	liServer *srv;
	liAngelLogOpen cb;
	gpointer data;
	GString *logname;
};

static void li_angel_log_open_cb(liAngelCall *acall, gpointer pctx, gboolean timeout, GString *error, GString *data, GArray *fds) {
	angel_log_cb_ctx ctx = * (angel_log_cb_ctx*) pctx;
	liServer *srv = ctx.srv;
	UNUSED(data);

	li_angel_call_free(acall);
	g_slice_free(angel_log_cb_ctx, pctx);

	if (timeout) {
		ERROR(srv, "Couldn't open log file '%s': timeout", ctx.logname->str);
		goto failed;
	}

	if (error->len > 0) {
		ERROR(srv, "Couldn't open log file '%s': %s", ctx.logname->str, error->str);
		goto failed;
	}

	if (NULL == fds || fds->len != 1) {
		ERROR(srv, "Couldn't open log file '%s': no or too many filedescriptors (%i)", ctx.logname->str, (int) (NULL == fds ? 0 : fds->len));
		goto failed;
	}

	ctx.cb(srv, g_array_index(fds, int, 0), ctx.data);
	g_array_set_size(fds, 0);

	goto cleanup;

failed:
	ctx.cb(srv, -1, ctx.data);

cleanup:
	g_string_free(ctx.logname, TRUE);
}

void li_angel_log_open_file(liServer *srv, GString *filename, liAngelLogOpen cb, gpointer data) {
	if (srv->acon) {
		liAngelCall *acall = li_angel_call_new(li_angel_log_open_cb, 10.0);
		angel_log_cb_ctx *ctx = g_slice_new0(angel_log_cb_ctx);
		GError *err = NULL;

		ctx->srv = srv;
		ctx->cb = cb;
		ctx->data = data;
		ctx->logname = g_string_new_len(GSTR_LEN(filename));

		acall->context = ctx;
		if (!li_angel_send_call(srv->acon, CONST_STR_LEN("core"), CONST_STR_LEN("log-open-file"), acall, g_string_new_len(GSTR_LEN(filename)), &err)) {
			ERROR(srv, "couldn't send call: %s", err->message);
			g_error_free(err);
		}
	} else {
		int fd = li_angel_fake_log_open_file(srv, filename);
		cb(srv, fd, data);
	}
}

void li_angel_log_open_pipe(liServer *srv, GString *pipename, liAngelLogOpen cb, gpointer data) {
	if (srv->acon) {
		liAngelCall *acall = li_angel_call_new(li_angel_log_open_cb, 10.0);
		angel_log_cb_ctx *ctx = g_slice_new0(angel_log_cb_ctx);
		GError *err = NULL;

		ctx->srv = srv;
		ctx->cb = cb;
		ctx->data = data;
		ctx->logname = g_string_new_len(GSTR_LEN(pipename));

		acall->context = ctx;
		if (!li_angel_send_call(srv->acon, CONST_STR_LEN("core"), CONST_STR_LEN("log-open-pipe"), acall, g_string_new_len(GSTR_LEN(pipename)), &err)) {
			ERROR(srv, "couldn't send call: %s", err->message);
			g_error_free(err);
		}
	} else {
		ERROR(srv, "angel required for: %s", "log-open-pipe");
	}
}

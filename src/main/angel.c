
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
	exit(1);
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
		liAngelCall *acall = li_angel_call_new(li_angel_listen_cb, 3.0);
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
		int fd = angel_fake_listen(srv, str);
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
	angel_fake_log(srv, str);
}

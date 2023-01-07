/*
 * mod_scgi - connect to SCGI backends for generating response content
 *
 * Author:
 *     Copyright (c) 2013 Stefan Bühler
 */

#include <lighttpd/base.h>
#include <lighttpd/plugin_core.h>
#include <lighttpd/backends.h>
#include <lighttpd/stream_http_response.h>


LI_API gboolean mod_scgi_init(liModules *mods, liModule *mod);
LI_API gboolean mod_scgi_free(liModules *mods, liModule *mod);


typedef struct scgi_connection scgi_connection;
typedef struct scgi_context scgi_context;

struct scgi_context {
	gint refcount;

	liBackendPool *pool;

	GString *socket_str;
};


struct scgi_connection {
	scgi_context *ctx;
	liBackendConnection *bcon;
	gpointer simple_socket_data;
};

/**********************************************************************************/

static gboolean append_key_value_pair(GByteArray *a, const gchar *key, size_t keylen, const gchar *val, size_t valuelen) {
	const guint8 z = 0;
	g_byte_array_append(a, (const guint8*) key, keylen);
	g_byte_array_append(a, &z, 1);
	g_byte_array_append(a, (const guint8*) val, valuelen);
	g_byte_array_append(a, &z, 1);
	return TRUE;
}

static void cgi_add_cb(gpointer param, const gchar *key, size_t keylen, const gchar *val, size_t valuelen) {
	GByteArray *a = (GByteArray*) param;
	append_key_value_pair(a, key, keylen, val, valuelen);
}

static void scgi_send_env(liVRequest *vr, liChunkQueue *out) {
	GByteArray *buf = g_byte_array_sized_new(0);
	liEnvironmentDup *envdup;
	GString *tmp = vr->wrk->tmp_str;
	GString *env_scgi_value;

	g_assert(vr->request.content_length >= 0);

	envdup = li_environment_make_dup(&vr->env);
	env_scgi_value = li_environment_dup_pop(envdup, CONST_STR_LEN("SCGI"));
	li_environment_dup2cgi(vr, envdup, cgi_add_cb, buf);

	if (NULL != env_scgi_value) {
		append_key_value_pair(buf, CONST_STR_LEN("SCGI"), GSTR_LEN(env_scgi_value));
	} else {
		append_key_value_pair(buf, CONST_STR_LEN("SCGI"), CONST_STR_LEN("1"));
	}

	g_string_printf(tmp, "%u:", buf->len);
	li_chunkqueue_append_mem(out, GSTR_LEN(tmp));
	{
		const guint8 c = ',';
		g_byte_array_append(buf, &c, 1);
	}
	li_chunkqueue_append_bytearr(out, buf);
}

/**********************************************************************************/

static void scgi_backend_free(liBackendPool *bpool) {
	liBackendConfig *config = (liBackendConfig*) bpool->config;
	li_sockaddr_clear(&config->sock_addr);
	g_slice_free(liBackendConfig, config);
}

static liBackendCallbacks scgi_backend_cbs = {
	/* backend_detach_thread */ NULL, 
	/* backend_attach_thread */ NULL,
	/* backend_new */ NULL,
	/* backend_close */ NULL,
	scgi_backend_free
};


static scgi_context* scgi_context_new(liServer *srv, GString *dest_socket) {
	liSocketAddress saddr;
	scgi_context* ctx;
	liBackendConfig *config;

	saddr = li_sockaddr_from_string(dest_socket, 0);
	if (NULL == saddr.addr_up.raw) {
		ERROR(srv, "Invalid socket address '%s'", dest_socket->str);
		return NULL;
	}

	config = g_slice_new0(liBackendConfig);
	config->callbacks = &scgi_backend_cbs;
	config->sock_addr = saddr;
	config->max_connections = 0;
	config->idle_timeout = 5;
	config->connect_timeout = 5;
	config->wait_timeout = 5;
	config->disable_time = 0;
	config->max_requests = 1;
	config->watch_for_close = TRUE;

	ctx = g_slice_new0(scgi_context);
	ctx->refcount = 1;
	ctx->pool = li_backend_pool_new(config);
	ctx->socket_str = g_string_new_len(GSTR_LEN(dest_socket));

	return ctx;
}

static void scgi_context_release(scgi_context *ctx) {
	if (!ctx) return;
	LI_FORCE_ASSERT(g_atomic_int_get(&ctx->refcount) > 0);
	if (g_atomic_int_dec_and_test(&ctx->refcount)) {
		li_backend_pool_free(ctx->pool);
		g_string_free(ctx->socket_str, TRUE);
		g_slice_free(scgi_context, ctx);
	}
}

static void scgi_context_acquire(scgi_context *ctx) {
	LI_FORCE_ASSERT(g_atomic_int_get(&ctx->refcount) > 0);
	g_atomic_int_inc(&ctx->refcount);
}


static void scgi_io_cb(liIOStream *stream, liIOStreamEvent event) {
	scgi_connection *con = stream->data;
	liWorker *wrk = li_worker_from_iostream(stream);

	li_stream_simple_socket_io_cb_with_context(stream, event, &con->simple_socket_data);

	switch (event) {
	case LI_IOSTREAM_DESTROY:
		li_stream_simple_socket_close(stream, FALSE);
		li_event_io_set_fd(&con->bcon->watcher, -1);

		li_backend_put(wrk, con->ctx->pool, con->bcon, TRUE);
		con->bcon = NULL;

		scgi_context_release(con->ctx);
		g_slice_free(scgi_connection, con);

		stream->data = NULL;
		return;
	default:
		break;
	}

	if ((NULL == stream->stream_in.out || stream->stream_in.out->is_closed) &&
		!(NULL == stream->stream_out.out || stream->stream_out.out->is_closed)) {
		stream->stream_out.out->is_closed = TRUE;
		li_stream_again_later(&stream->stream_out);
	}
}

static void scgi_connection_new(liVRequest *vr, liBackendConnection *bcon, scgi_context *ctx) {
	scgi_connection* scon = g_slice_new0(scgi_connection);
	liIOStream *iostream;
	liStream *outplug;
	liStream *http_out;

	scgi_context_acquire(ctx);
	scon->ctx = ctx;
	scon->bcon = bcon;
	iostream = li_iostream_new(vr->wrk, li_event_io_fd(&bcon->watcher), scgi_io_cb, scon);

	/* insert scgi header before actual data */
	outplug = li_stream_plug_new(&vr->wrk->loop);

	li_stream_connect(outplug, &iostream->stream_out);

	scgi_send_env(vr, outplug->out);
	li_stream_notify_later(outplug);

	http_out = li_stream_http_response_handle(&iostream->stream_in, vr, TRUE, FALSE, FALSE);

	li_vrequest_handle_indirect(vr, NULL);
	li_vrequest_indirect_connect(vr, outplug, http_out);

	li_iostream_release(iostream);
	li_stream_release(outplug);
	li_stream_release(http_out);
}

/**********************************************************************************/

static liHandlerResult scgi_handle_abort(liVRequest *vr, gpointer param, gpointer context) {
	scgi_context *ctx = (scgi_context*) param;
	liBackendWait *bwait = context;

	if (bwait != NULL) {
		li_backend_wait_stop(vr, ctx->pool, &bwait);
	}

	return LI_HANDLER_GO_ON;
}

static liHandlerResult scgi_handle(liVRequest *vr, gpointer param, gpointer *context) {
	liBackendWait *bwait = (liBackendWait*) *context;
	liBackendConnection *bcon = NULL;
	scgi_context *ctx = (scgi_context*) param;
	liBackendResult bres;

	if (li_vrequest_is_handled(vr)) return LI_HANDLER_GO_ON;

	LI_VREQUEST_WAIT_FOR_REQUEST_BODY(vr);

	if (vr->request.content_length < 0) {
		VR_ERROR(vr, "%s", "scgi can't handle progressive uploads. enable request body buffering!");
		return LI_HANDLER_ERROR;
	}

	bres = li_backend_get(vr, ctx->pool, &bcon, &bwait);
	*context = bwait;
	switch (bres) {
	case LI_BACKEND_SUCCESS:
		LI_FORCE_ASSERT(NULL == bwait);
		LI_FORCE_ASSERT(NULL != bcon);
		break;
	case LI_BACKEND_WAIT:
		LI_FORCE_ASSERT(NULL != bwait);
		return LI_HANDLER_WAIT_FOR_EVENT;
	case LI_BACKEND_TIMEOUT:
		li_vrequest_backend_dead(vr);
		return LI_HANDLER_GO_ON;
	}

	scgi_connection_new(vr, bcon, ctx);
	return LI_HANDLER_GO_ON;
}

static void scgi_free(liServer *srv, gpointer param) {
	scgi_context *ctx = (scgi_context*) param;
	UNUSED(srv);

	scgi_context_release(ctx);
}

static liAction* scgi_create(liServer *srv, liWorker *wrk, liPlugin* p, liValue *val, gpointer userdata) {
	scgi_context *ctx;
	UNUSED(wrk); UNUSED(userdata); UNUSED(p);

	val = li_value_get_single_argument(val);

	if (LI_VALUE_STRING != li_value_type(val)) {
		ERROR(srv, "%s", "scgi expects a string as parameter");
		return FALSE;
	}

	ctx = scgi_context_new(srv, val->data.string);
	if (NULL == ctx) return NULL;

	return li_action_new_function(scgi_handle, scgi_handle_abort, scgi_free, ctx);
}

static const liPluginOption options[] = {
	{ NULL, 0, 0, NULL }
};

static const liPluginAction actions[] = {
	{ "scgi", scgi_create, NULL },

	{ NULL, NULL, NULL }
};

static const liPluginSetup setups[] = {
	{ NULL, NULL, NULL }
};


static void plugin_init(liServer *srv, liPlugin *p, gpointer userdata) {
	UNUSED(srv); UNUSED(userdata);

	p->options = options;
	p->actions = actions;
	p->setups = setups;
}


gboolean mod_scgi_init(liModules *mods, liModule *mod) {
	MODULE_VERSION_CHECK(mods);

	mod->config = li_plugin_register(mods->main, "mod_scgi", plugin_init, NULL);

	return mod->config != NULL;
}

gboolean mod_scgi_free(liModules *mods, liModule *mod) {
	if (mod->config)
		li_plugin_free(mods->main, mod->config);

	return TRUE;
}

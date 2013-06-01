/*
 * mod_proxy - connect to proxy backends for generating content
 *
 * Description:
 *     mod_proxy connects to a backend over tcp or unix sockets
 *
 * Setups:
 *     none
 * Options:
 *     none
 * Actions:
 *     proxy <socket>  - connect to backend at <socket>
 *         socket: string, either "ip:port" or "unix:/path"
 *
 * Example config:
 *     proxy "127.0.0.1:9090"
 *
 * TODO:
 *     - keep-alive connections
 *
 * Author:
 *     Copyright (c) 2013 Stefan Bühler
 */

#include <lighttpd/base.h>
#include <lighttpd/plugin_core.h>
#include <lighttpd/backends.h>
#include <lighttpd/stream_http_response.h>


LI_API gboolean mod_proxy_init(liModules *mods, liModule *mod);
LI_API gboolean mod_proxy_free(liModules *mods, liModule *mod);


typedef struct proxy_connection proxy_connection;
typedef struct proxy_context proxy_context;

struct proxy_context {
	gint refcount;

	liBackendPool *pool;

	GString *socket_str;
};


struct proxy_connection {
	proxy_context *ctx;
	liBackendConnection *bcon;
	gpointer simple_socket_data;
};

/**********************************************************************************/

static void proxy_send_headers(liVRequest *vr, liChunkQueue *out) {
	GString *head = g_string_sized_new(4095);
	liHttpHeader *header;
	GList *iter;
	gchar *enc_path;
	liHttpHeaderTokenizer header_tokenizer;
	GString *tmp_str = vr->wrk->tmp_str;

	g_string_append_len(head, GSTR_LEN(vr->request.http_method_str));
	g_string_append_len(head, CONST_STR_LEN(" "));

	enc_path = g_uri_escape_string(vr->request.uri.path->str, "/", FALSE);
	g_string_append(head, enc_path);
	g_free(enc_path);

	if (vr->request.uri.query->len > 0) {
		g_string_append_len(head, CONST_STR_LEN("?"));
		g_string_append_len(head, GSTR_LEN(vr->request.uri.query));
	}

	switch (vr->request.http_version) {
	case LI_HTTP_VERSION_1_1:
		/* g_string_append_len(head, CONST_STR_LEN(" HTTP/1.1\r\n")); */
		g_string_append_len(head, CONST_STR_LEN(" HTTP/1.0\r\n"));
		break;
	case LI_HTTP_VERSION_1_0:
	default:
		g_string_append_len(head, CONST_STR_LEN(" HTTP/1.0\r\n"));
		break;
	}

	li_http_header_tokenizer_start(&header_tokenizer, vr->request.headers, CONST_STR_LEN("Connection"));
	while (li_http_header_tokenizer_next(&header_tokenizer, tmp_str)) {
		if (0 == g_ascii_strcasecmp(tmp_str->str, "Upgrade")) {
			g_string_append_len(head, CONST_STR_LEN("Connection: Upgrade\r\n"));
		}
	}

	if (vr->request.content_length > 0) {
		g_string_append_printf(head, "Content-Length: %" LI_GOFFSET_MODIFIER "i\r\n", vr->request.content_length);
	}

	for (iter = g_queue_peek_head_link(&vr->request.headers->entries); iter; iter = g_list_next(iter)) {
		header = (liHttpHeader*) iter->data;
		if (li_http_header_key_is(header, CONST_STR_LEN("Content-Length"))) continue;
		if (li_http_header_key_is(header, CONST_STR_LEN("Transfer-Encoding"))) continue;
		if (li_http_header_key_is(header, CONST_STR_LEN("TE"))) continue;
		if (li_http_header_key_is(header, CONST_STR_LEN("Connection"))) continue;
		if (li_http_header_key_is(header, CONST_STR_LEN("Proxy-Connection"))) continue;
		if (li_http_header_key_is(header, CONST_STR_LEN("X-Forwarded-Proto"))) continue;
		if (li_http_header_key_is(header, CONST_STR_LEN("X-Forwarded-For"))) continue;
		g_string_append_len(head, GSTR_LEN(header->data));
		g_string_append_len(head, CONST_STR_LEN("\r\n"));
	}

	g_string_append_len(head, CONST_STR_LEN("X-Forwarded-For: "));
	g_string_append_len(head, GSTR_LEN(vr->coninfo->remote_addr_str));
	g_string_append_len(head, CONST_STR_LEN("\r\n"));

	if (vr->coninfo->is_ssl) {
		g_string_append_len(head, CONST_STR_LEN("X-Forwarded-Proto: https\r\n"));
	} else {
		g_string_append_len(head, CONST_STR_LEN("X-Forwarded-Proto: http\r\n"));
	}

	/* terminate http header */
	g_string_append_len(head, CONST_STR_LEN("\r\n"));

	li_chunkqueue_append_string(out, head);
}

/**********************************************************************************/

static void proxy_backend_free(liBackendPool *bpool) {
	liBackendConfig *config = (liBackendConfig*) bpool->config;
	li_sockaddr_clear(&config->sock_addr);
	g_slice_free(liBackendConfig, config);
}

static liBackendCallbacks proxy_backend_cbs = {
	/* backend_detach_thread */ NULL, 
	/* backend_attach_thread */ NULL,
	/* backend_new */ NULL,
	/* backend_close */ NULL,
	proxy_backend_free
};


static proxy_context* proxy_context_new(liServer *srv, GString *dest_socket) {
	liSocketAddress saddr;
	proxy_context* ctx;
	liBackendConfig *config;

	saddr = li_sockaddr_from_string(dest_socket, 0);
	if (NULL == saddr.addr) {
		ERROR(srv, "Invalid socket address '%s'", dest_socket->str);
		return NULL;
	}

	config = g_slice_new0(liBackendConfig);
	config->callbacks = &proxy_backend_cbs;
	config->sock_addr = saddr;
	config->max_connections = 0;
	config->idle_timeout = 5;
	config->connect_timeout = 5;
	config->wait_timeout = 5;
	config->disable_time = 0;
	config->max_requests = 1;
	config->watch_for_close = TRUE;

	ctx = g_slice_new0(proxy_context);
	ctx->refcount = 1;
	ctx->pool = li_backend_pool_new(config);
	ctx->socket_str = g_string_new_len(GSTR_LEN(dest_socket));

	return ctx;
}

static void proxy_context_release(proxy_context *ctx) {
	if (!ctx) return;
	assert(g_atomic_int_get(&ctx->refcount) > 0);
	if (g_atomic_int_dec_and_test(&ctx->refcount)) {
		li_backend_pool_free(ctx->pool);
		g_string_free(ctx->socket_str, TRUE);
		g_slice_free(proxy_context, ctx);
	}
}

static void proxy_context_acquire(proxy_context *ctx) {
	assert(g_atomic_int_get(&ctx->refcount) > 0);
	g_atomic_int_inc(&ctx->refcount);
}


static void proxy_io_cb(liIOStream *stream, liIOStreamEvent event) {
	proxy_connection *con = stream->data;
	liWorker *wrk = li_worker_from_iostream(stream);

	li_stream_simple_socket_io_cb_with_context(stream, event, &con->simple_socket_data);

	switch (event) {
	case LI_IOSTREAM_DESTROY:
		li_stream_simple_socket_close(stream, FALSE);
		li_event_io_set_fd(&con->bcon->watcher, -1);

		li_backend_put(wrk, con->ctx->pool, con->bcon, TRUE);
		con->bcon = NULL;

		proxy_context_release(con->ctx);
		g_slice_free(proxy_connection, con);

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

static void proxy_connection_new(liVRequest *vr, liBackendConnection *bcon, proxy_context *ctx) {
	proxy_connection* scon = g_slice_new0(proxy_connection);
	liIOStream *iostream;
	liStream *outplug;
	liStream *http_out;

	proxy_context_acquire(ctx);
	scon->ctx = ctx;
	scon->bcon = bcon;
	iostream = li_iostream_new(vr->wrk, li_event_io_fd(&bcon->watcher), proxy_io_cb, scon);

	/* insert proxy header before actual data */
	outplug = li_stream_plug_new(&vr->wrk->loop);

	li_stream_connect(outplug, &iostream->stream_out);

	proxy_send_headers(vr, outplug->out);
	li_stream_notify_later(outplug);

	http_out = li_stream_http_response_handle(&iostream->stream_in, vr, TRUE, FALSE);

	li_vrequest_handle_indirect(vr, NULL);
	li_vrequest_indirect_connect(vr, outplug, http_out);

	li_iostream_release(iostream);
	li_stream_release(outplug);
	li_stream_release(http_out);
}

/**********************************************************************************/

static liHandlerResult proxy_handle_abort(liVRequest *vr, gpointer param, gpointer context) {
	proxy_context *ctx = (proxy_context*) param;
	liBackendWait *bwait = context;

	if (bwait != NULL) {
		li_backend_wait_stop(vr, ctx->pool, &bwait);
	}

	return LI_HANDLER_GO_ON;
}

static liHandlerResult proxy_handle(liVRequest *vr, gpointer param, gpointer *context) {
	liBackendWait *bwait = (liBackendWait*) *context;
	liBackendConnection *bcon = NULL;
	proxy_context *ctx = (proxy_context*) param;

	if (li_vrequest_is_handled(vr)) return LI_HANDLER_GO_ON;

	LI_VREQUEST_WAIT_FOR_REQUEST_BODY(vr);

	if (vr->request.content_length < 0) {
		VR_ERROR(vr, "%s", "proxy can't handle progressive uploads yet. enable request body buffering!");
		return LI_HANDLER_ERROR;
	}

	switch (li_backend_get(vr, ctx->pool, &bcon, &bwait)) {
	case LI_BACKEND_SUCCESS:
		assert(NULL == bwait);
		assert(NULL != bcon);
		*context = bwait;
		break;
	case LI_BACKEND_WAIT:
		assert(NULL != bwait);
		*context = bwait;

		return LI_HANDLER_WAIT_FOR_EVENT;
	case LI_BACKEND_TIMEOUT:
		li_vrequest_backend_dead(vr);
		return LI_HANDLER_GO_ON;
	}

	proxy_connection_new(vr, bcon, ctx);
	return LI_HANDLER_GO_ON;
}

static void proxy_free(liServer *srv, gpointer param) {
	proxy_context *ctx = (proxy_context*) param;
	UNUSED(srv);

	proxy_context_release(ctx);
}

static liAction* proxy_create(liServer *srv, liWorker *wrk, liPlugin* p, liValue *val, gpointer userdata) {
	proxy_context *ctx;
	UNUSED(wrk); UNUSED(userdata); UNUSED(p);

	if (val->type != LI_VALUE_STRING) {
		ERROR(srv, "%s", "proxy expects a string as parameter");
		return FALSE;
	}

	ctx = proxy_context_new(srv, val->data.string);
	if (NULL == ctx) return NULL;

	return li_action_new_function(proxy_handle, proxy_handle_abort, proxy_free, ctx);
}

static const liPluginOption options[] = {
	{ NULL, 0, 0, NULL }
};

static const liPluginAction actions[] = {
	{ "proxy", proxy_create, NULL },

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


gboolean mod_proxy_init(liModules *mods, liModule *mod) {
	MODULE_VERSION_CHECK(mods);

	mod->config = li_plugin_register(mods->main, "mod_proxy", plugin_init, NULL);

	return mod->config != NULL;
}

gboolean mod_proxy_free(liModules *mods, liModule *mod) {
	if (mod->config)
		li_plugin_free(mods->main, mod->config);

	return TRUE;
}

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

static void scgi_env_add(GByteArray *buf, liEnvironmentDup *envdup, const gchar *key, size_t keylen, const gchar *val, size_t valuelen) {
	GString *sval;

	if (NULL != (sval = li_environment_dup_pop(envdup, key, keylen))) {
		append_key_value_pair(buf, key, keylen, GSTR_LEN(sval));
	} else {
		append_key_value_pair(buf, key, keylen, val, valuelen);
	}
}

static void scgi_env_create(liVRequest *vr, liEnvironmentDup *envdup, GByteArray* buf) {
	liConInfo *coninfo = vr->coninfo;
	GString *tmp = vr->wrk->tmp_str;

	g_assert(vr->request.content_length >= 0);

	if (vr->request.content_length >= 0) {
		g_string_printf(tmp, "%" LI_GOFFSET_MODIFIER "i", vr->request.content_length);
		scgi_env_add(buf, envdup, CONST_STR_LEN("CONTENT_LENGTH"), GSTR_LEN(tmp));
	}

	scgi_env_add(buf, envdup, CONST_STR_LEN("SCGI"), CONST_STR_LEN("1"));


	scgi_env_add(buf, envdup, CONST_STR_LEN("SERVER_SOFTWARE"), GSTR_LEN(CORE_OPTIONPTR(LI_CORE_OPTION_SERVER_TAG).string));
	scgi_env_add(buf, envdup, CONST_STR_LEN("SERVER_NAME"), GSTR_LEN(vr->request.uri.host));
	scgi_env_add(buf, envdup, CONST_STR_LEN("GATEWAY_INTERFACE"), CONST_STR_LEN("CGI/1.1"));
	{
		guint port = 0;
		switch (coninfo->local_addr.addr->plain.sa_family) {
		case AF_INET: port = coninfo->local_addr.addr->ipv4.sin_port; break;
#ifdef HAVE_IPV6
		case AF_INET6: port = coninfo->local_addr.addr->ipv6.sin6_port; break;
#endif
		}
		if (port) {
			g_string_printf(tmp, "%u", htons(port));
			scgi_env_add(buf, envdup, CONST_STR_LEN("SERVER_PORT"), GSTR_LEN(tmp));
		}
	}
	scgi_env_add(buf, envdup, CONST_STR_LEN("SERVER_ADDR"), GSTR_LEN(coninfo->local_addr_str));

	{
		guint port = 0;
		switch (coninfo->remote_addr.addr->plain.sa_family) {
		case AF_INET: port = coninfo->remote_addr.addr->ipv4.sin_port; break;
#ifdef HAVE_IPV6
		case AF_INET6: port = coninfo->remote_addr.addr->ipv6.sin6_port; break;
#endif
		}
		if (port) {
			g_string_printf(tmp, "%u", htons(port));
			scgi_env_add(buf, envdup, CONST_STR_LEN("REMOTE_PORT"), GSTR_LEN(tmp));
		}
	}
	scgi_env_add(buf, envdup, CONST_STR_LEN("REMOTE_ADDR"), GSTR_LEN(coninfo->remote_addr_str));

	scgi_env_add(buf, envdup, CONST_STR_LEN("SCRIPT_NAME"), GSTR_LEN(vr->request.uri.path));

	scgi_env_add(buf, envdup, CONST_STR_LEN("PATH_INFO"), GSTR_LEN(vr->physical.pathinfo));
	if (vr->physical.pathinfo->len) {
		g_string_truncate(tmp, 0);
		g_string_append_len(tmp, GSTR_LEN(vr->physical.doc_root));
		g_string_append_len(tmp, GSTR_LEN(vr->physical.pathinfo));
		scgi_env_add(buf, envdup, CONST_STR_LEN("PATH_TRANSLATED"), GSTR_LEN(tmp));
	}

	scgi_env_add(buf, envdup, CONST_STR_LEN("SCRIPT_FILENAME"), GSTR_LEN(vr->physical.path));
	scgi_env_add(buf, envdup, CONST_STR_LEN("DOCUMENT_ROOT"), GSTR_LEN(vr->physical.doc_root));

	scgi_env_add(buf, envdup, CONST_STR_LEN("REQUEST_URI"), GSTR_LEN(vr->request.uri.raw_orig_path));
	if (!g_string_equal(vr->request.uri.raw_orig_path, vr->request.uri.raw_path)) {
		scgi_env_add(buf, envdup, CONST_STR_LEN("REDIRECT_URI"), GSTR_LEN(vr->request.uri.raw_path));
	}
	scgi_env_add(buf, envdup, CONST_STR_LEN("QUERY_STRING"), GSTR_LEN(vr->request.uri.query));

	scgi_env_add(buf, envdup, CONST_STR_LEN("REQUEST_METHOD"), GSTR_LEN(vr->request.http_method_str));
	scgi_env_add(buf, envdup, CONST_STR_LEN("REDIRECT_STATUS"), CONST_STR_LEN("200")); /* if php is compiled with --force-redirect */
	switch (vr->request.http_version) {
	case LI_HTTP_VERSION_1_1:
		scgi_env_add(buf, envdup, CONST_STR_LEN("SERVER_PROTOCOL"), CONST_STR_LEN("HTTP/1.1"));
		break;
	case LI_HTTP_VERSION_1_0:
	default:
		scgi_env_add(buf, envdup, CONST_STR_LEN("SERVER_PROTOCOL"), CONST_STR_LEN("HTTP/1.0"));
		break;
	}

	if (coninfo->is_ssl) {
		scgi_env_add(buf, envdup, CONST_STR_LEN("HTTPS"), CONST_STR_LEN("on"));
	}
}

static void fix_header_name(GString *str) {
	guint i, len = str->len;
	gchar *s = str->str;
	for (i = 0; i < len; i++) {
		if (g_ascii_isalpha(s[i])) {
			s[i] = g_ascii_toupper(s[i]);
		} else if (!g_ascii_isdigit(s[i])) {
			s[i] = '_';
		}
	}
}

static void scgi_send_env(liVRequest *vr, liChunkQueue *out) {
	GByteArray *buf = g_byte_array_sized_new(0);
	liEnvironmentDup *envdup;
	GString *tmp = vr->wrk->tmp_str;

	envdup = li_environment_make_dup(&vr->env);
	scgi_env_create(vr, envdup, buf);

	{
		GList *i;

		for (i = vr->request.headers->entries.head; NULL != i; i = i->next) {
			liHttpHeader *h = (liHttpHeader*) i->data;
			const GString hkey = li_const_gstring(h->data->str, h->keylen);
			g_string_truncate(tmp, 0);
			if (!li_strncase_equal(&hkey, CONST_STR_LEN("CONTENT-TYPE"))) {
				g_string_append_len(tmp, CONST_STR_LEN("HTTP_"));
			}
			g_string_append_len(tmp, h->data->str, h->keylen);
			fix_header_name(tmp);

			scgi_env_add(buf, envdup, GSTR_LEN(tmp), h->data->str + h->keylen+2, h->data->len - (h->keylen+2));
		}
	}

	{
		GHashTableIter i;
		gpointer key, val;

		g_hash_table_iter_init(&i, envdup->table);
		while (g_hash_table_iter_next(&i, &key, &val)) {
			append_key_value_pair(buf, GSTR_LEN((GString*) key), GSTR_LEN((GString*) val));
		}
	}

	li_environment_dup_free(envdup);

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
	if (NULL == saddr.addr) {
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

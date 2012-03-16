/*
 * mod_scgi - connect to scgi backends for generating content
 *
 * Description:
 *     mod_scgi connects to a backend over tcp or unix sockets
 *
 * Setups:
 *     none
 * Options:
 *     none
 * Actions:
 *     scgi <socket>  - connect to backend at <socket>
 *         socket: string, either "ip:port" or "unix:/path"
 *
 * Example config:
 *     scgi "127.0.0.1:9090"
 *
 * Author:
 *     Copyright (c) 2009 Stefan Bühler
 */

#include <lighttpd/base.h>
#include <lighttpd/plugin_core.h>


LI_API gboolean mod_scgi_init(liModules *mods, liModule *mod);
LI_API gboolean mod_scgi_free(liModules *mods, liModule *mod);


typedef struct scgi_connection scgi_connection;
typedef struct scgi_context scgi_context;


typedef enum {
	SS_WAIT_FOR_REQUEST,
	SS_CONNECT,
	SS_CONNECTING,
	SS_CONNECTED,
	SS_DONE
} scgi_state;


struct scgi_connection {
	scgi_context *ctx;
	liVRequest *vr;
	scgi_state state;
	int fd;
	ev_io fd_watcher;
	liChunkQueue *scgi_in, *scgi_out;
	liBuffer *scgi_in_buffer;

	liHttpResponseCtx parse_response_ctx;
	gboolean response_headers_finished;
};

struct scgi_context {
	gint refcount;
	liSocketAddress socket;
	GString *socket_str;
	guint timeout;
	liPlugin *plugin;
};

/**********************************************************************************/

static scgi_context* scgi_context_new(liServer *srv, liPlugin *p, GString *dest_socket) {
	liSocketAddress saddr;
	scgi_context* ctx;
	saddr = li_sockaddr_from_string(dest_socket, 0);
	if (NULL == saddr.addr) {
		ERROR(srv, "Invalid socket address '%s'", dest_socket->str);
		return NULL;
	}
	ctx = g_slice_new0(scgi_context);
	ctx->refcount = 1;
	ctx->socket = saddr;
	ctx->timeout = 5;
	ctx->plugin = p;
	ctx->socket_str = g_string_new_len(GSTR_LEN(dest_socket));
	return ctx;
}

static void scgi_context_release(scgi_context *ctx) {
	if (!ctx) return;
	assert(g_atomic_int_get(&ctx->refcount) > 0);
	if (g_atomic_int_dec_and_test(&ctx->refcount)) {
		li_sockaddr_clear(&ctx->socket);
		g_string_free(ctx->socket_str, TRUE);
		g_slice_free(scgi_context, ctx);
	}
}

static void scgi_context_acquire(scgi_context *ctx) {
	assert(g_atomic_int_get(&ctx->refcount) > 0);
	g_atomic_int_inc(&ctx->refcount);
}

static void scgi_fd_cb(struct ev_loop *loop, ev_io *w, int revents);

static scgi_connection* scgi_connection_new(liVRequest *vr, scgi_context *ctx) {
	scgi_connection* scon = g_slice_new0(scgi_connection);

	scgi_context_acquire(ctx);
	scon->ctx = ctx;
	scon->vr = vr;
	scon->fd = -1;
	ev_init(&scon->fd_watcher, scgi_fd_cb);
	ev_io_set(&scon->fd_watcher, -1, 0);
	scon->fd_watcher.data = scon;
	scon->scgi_in = li_chunkqueue_new();
	scon->scgi_out = li_chunkqueue_new();
	scon->state = SS_WAIT_FOR_REQUEST;
	li_http_response_parser_init(&scon->parse_response_ctx, &vr->response, scon->scgi_in, TRUE, FALSE);
	scon->response_headers_finished = FALSE;
	return scon;
}

static void scgi_connection_free(scgi_connection *scon) {
	liVRequest *vr;
	if (!scon) return;

	vr = scon->vr;
	ev_io_stop(vr->wrk->loop, &scon->fd_watcher);
	scgi_context_release(scon->ctx);
	if (scon->fd != -1) close(scon->fd);
	li_vrequest_backend_finished(vr);

	li_chunkqueue_free(scon->scgi_in);
	li_chunkqueue_free(scon->scgi_out);
	li_buffer_release(scon->scgi_in_buffer);

	li_http_response_parser_clear(&scon->parse_response_ctx);

	g_slice_free(scgi_connection, scon);
}

/**********************************************************************************/
/* scgi stream helper */

static void stream_send_chunks(liChunkQueue *out, liChunkQueue *in) {
	li_chunkqueue_steal_all(out, in);

	if (in->is_closed && !out->is_closed) {
		out->is_closed = TRUE;
	}
}

static gboolean append_key_value_pair(GByteArray *a, const gchar *key, size_t keylen, const gchar *val, size_t valuelen) {
	const guint8 z = 0;
	g_byte_array_append(a, (const guint8*) key, keylen);
	g_byte_array_append(a, &z, 1);
	g_byte_array_append(a, (const guint8*) val, valuelen);
	g_byte_array_append(a, &z, 1);
	return TRUE;
}

/**********************************************************************************/

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
		g_string_printf(tmp, "%" L_GOFFSET_MODIFIER "i", vr->request.content_length);
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

static void scgi_send_env(liVRequest *vr, scgi_connection *scon) {
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
	li_chunkqueue_append_mem(scon->scgi_out, GSTR_LEN(tmp));
	{
		const guint8 c = ',';
		g_byte_array_append(buf, &c, 1);
	}
	li_chunkqueue_append_bytearr(scon->scgi_out, buf);
}

static void scgi_forward_request(liVRequest *vr, scgi_connection *scon) {
	stream_send_chunks(scon->scgi_out, vr->in);
	if (scon->scgi_out->length > 0)
		li_ev_io_add_events(vr->wrk->loop, &scon->fd_watcher, EV_WRITE);
}

/**********************************************************************************/

static liHandlerResult scgi_statemachine(liVRequest *vr, scgi_connection *scon);

static void scgi_fd_cb(struct ev_loop *loop, ev_io *w, int revents) {
	scgi_connection *scon = (scgi_connection*) w->data;

	if (scon->state == SS_CONNECTING) {
		if (LI_HANDLER_GO_ON != scgi_statemachine(scon->vr, scon)) {
			li_vrequest_error(scon->vr);
		}
		return;
	}

	if (revents & EV_READ) {
		if (scon->scgi_in->is_closed) {
			li_ev_io_rem_events(loop, w, EV_READ);
		} else {
			GError *err = NULL;
			switch (li_network_read(w->fd, scon->scgi_in, &scon->scgi_in_buffer, &err)) {
			case LI_NETWORK_STATUS_SUCCESS:
				break;
			case LI_NETWORK_STATUS_FATAL_ERROR:
				if (NULL != err) {
					VR_ERROR(scon->vr, "(%s) network read fatal error: %s", scon->ctx->socket_str->str, err->message);
					g_error_free(err);
				} else {
					VR_ERROR(scon->vr, "(%s) network read fatal error", scon->ctx->socket_str->str);
				}
				li_vrequest_error(scon->vr);
				return;
			case LI_NETWORK_STATUS_CONNECTION_CLOSE:
				scon->scgi_in->is_closed = TRUE;
				ev_io_stop(loop, w);
				close(scon->fd);
				scon->fd = -1;
				li_vrequest_backend_finished(scon->vr);
				break;
			case LI_NETWORK_STATUS_WAIT_FOR_EVENT:
				break;
			}
		}
	}

	if (scon->fd != -1 && (revents & EV_WRITE)) {
		if (scon->scgi_out->length > 0) {
			GError *err = NULL;
			switch (li_network_write(w->fd, scon->scgi_out, 256*1024, &err)) {
			case LI_NETWORK_STATUS_SUCCESS:
				break;
			case LI_NETWORK_STATUS_FATAL_ERROR:
				if (NULL != err) {
					VR_ERROR(scon->vr, "(%s) network write fatal error: %s", scon->ctx->socket_str->str, err->message);
					g_error_free(err);
				} else {
					VR_ERROR(scon->vr, "(%s) network write fatal error", scon->ctx->socket_str->str);
				}
				li_vrequest_error(scon->vr);
				return;
			case LI_NETWORK_STATUS_CONNECTION_CLOSE:
				scon->scgi_in->is_closed = TRUE;
				ev_io_stop(loop, w);
				close(scon->fd);
				scon->fd = -1;
				li_vrequest_backend_finished(scon->vr);
				break;
			case LI_NETWORK_STATUS_WAIT_FOR_EVENT:
				break;
			}
		}
		if (scon->scgi_out->length == 0) {
			li_ev_io_rem_events(loop, w, EV_WRITE);
		}
	}

	if (!scon->response_headers_finished && LI_HANDLER_GO_ON == li_http_response_parse(scon->vr, &scon->parse_response_ctx)) {
		scon->response_headers_finished = TRUE;
		li_vrequest_handle_response_headers(scon->vr);
	}

	if (scon->response_headers_finished) {
		li_chunkqueue_steal_all(scon->vr->out, scon->scgi_in);
		scon->vr->out->is_closed = scon->scgi_in->is_closed;
		li_vrequest_handle_response_body(scon->vr);
	}

	/* only possible if we didn't found a header */
	if (scon->scgi_in->is_closed && !scon->vr->out->is_closed) {
		VR_ERROR(scon->vr, "(%s) unexpected end-of-file (perhaps the scgi process died)", scon->ctx->socket_str->str);
		li_vrequest_error(scon->vr);
	}
}

/**********************************************************************************/
/* state machine */

static void scgi_close(liVRequest *vr, liPlugin *p);

static liHandlerResult scgi_statemachine(liVRequest *vr, scgi_connection *scon) {
	liPlugin *p = scon->ctx->plugin;

	switch (scon->state) {
	case SS_WAIT_FOR_REQUEST:
		/* wait until we have either all data or the cqlimit is full */
		if (-1 == vr->request.content_length || vr->request.content_length != vr->in->length) {
			if (0 != li_chunkqueue_limit_available(vr->in))
				return LI_HANDLER_GO_ON;
			VR_ERROR(scon->vr, "%s", "mod_scgi doesn't support uploads without content-length, and the chunkqueue limit was hit");
			return LI_HANDLER_ERROR;
		}
		scon->state = SS_CONNECT;

		/* fall through */
	case SS_CONNECT:
		do {
			scon->fd = socket(scon->ctx->socket.addr->plain.sa_family, SOCK_STREAM, 0);
		} while (-1 == scon->fd && errno == EINTR);
		if (-1 == scon->fd) {
			if (errno == EMFILE) {
				li_server_out_of_fds(vr->wrk->srv);
			}
			VR_ERROR(vr, "Couldn't open socket: %s", g_strerror(errno));
			return LI_HANDLER_ERROR;
		}
		li_fd_init(scon->fd);
		ev_io_set(&scon->fd_watcher, scon->fd, EV_READ | EV_WRITE);
		ev_io_start(vr->wrk->loop, &scon->fd_watcher);

		/* fall through */
	case SS_CONNECTING:
		if (-1 == connect(scon->fd, &scon->ctx->socket.addr->plain, scon->ctx->socket.len)) {
			switch (errno) {
			case EINPROGRESS:
			case EALREADY:
			case EINTR:
				scon->state = SS_CONNECTING;
				return LI_HANDLER_GO_ON;
			case EAGAIN: /* backend overloaded */
				scgi_close(vr, p);
				li_vrequest_backend_overloaded(vr);
				return LI_HANDLER_GO_ON;
			case EISCONN:
				break;
			default:
				VR_ERROR(vr, "Couldn't connect to '%s': %s",
					li_sockaddr_to_string(scon->ctx->socket, vr->wrk->tmp_str, TRUE)->str,
					g_strerror(errno));
				scgi_close(vr, p);
				li_vrequest_backend_dead(vr);
				return LI_HANDLER_GO_ON;
			}
		}

		scon->state = SS_CONNECTED;

		/* prepare stream */
		scgi_send_env(vr, scon);

		/* fall through */
	case SS_CONNECTED:
		scgi_forward_request(vr, scon);
		break;

	case SS_DONE:
		break;
	}

	return LI_HANDLER_GO_ON;
}


/**********************************************************************************/

static liHandlerResult scgi_handle(liVRequest *vr, gpointer param, gpointer *context) {
	scgi_context *ctx = (scgi_context*) param;
	scgi_connection *scon;
	UNUSED(context);
	if (!li_vrequest_handle_indirect(vr, ctx->plugin)) return LI_HANDLER_GO_ON;

	scon = scgi_connection_new(vr, ctx);
	if (!scon) {
		return LI_HANDLER_ERROR;
	}
	g_ptr_array_index(vr->plugin_ctx, ctx->plugin->id) = scon;

	li_chunkqueue_set_limit(scon->scgi_in, vr->out->limit);
	li_chunkqueue_set_limit(scon->scgi_out, vr->in->limit);
	if (vr->out->limit) vr->out->limit->io_watcher = &scon->fd_watcher;

	return scgi_statemachine(vr, scon);
}


static liHandlerResult scgi_handle_request_body(liVRequest *vr, liPlugin *p) {
	scgi_connection *scon = (scgi_connection*) g_ptr_array_index(vr->plugin_ctx, p->id);
	if (!scon) return LI_HANDLER_ERROR;

	return scgi_statemachine(vr, scon);
}

static void scgi_close(liVRequest *vr, liPlugin *p) {
	scgi_connection *scon = (scgi_connection*) g_ptr_array_index(vr->plugin_ctx, p->id);
	g_ptr_array_index(vr->plugin_ctx, p->id) = NULL;
	if (scon) {
		if (vr->out->limit) vr->out->limit->io_watcher = NULL;
		scgi_connection_free(scon);
	}
}


static void scgi_free(liServer *srv, gpointer param) {
	scgi_context *ctx = (scgi_context*) param;
	UNUSED(srv);

	scgi_context_release(ctx);
}

static liAction* scgi_create(liServer *srv, liWorker *wrk, liPlugin* p, liValue *val, gpointer userdata) {
	scgi_context *ctx;
	UNUSED(wrk); UNUSED(userdata);

	if (val->type != LI_VALUE_STRING) {
		ERROR(srv, "%s", "scgi expects a string as parameter");
		return FALSE;
	}

	ctx = scgi_context_new(srv, p, val->data.string);
	if (!ctx) return NULL;

	return li_action_new_function(scgi_handle, NULL, scgi_free, ctx);
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

	p->handle_request_body = scgi_handle_request_body;
	p->handle_vrclose = scgi_close;
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

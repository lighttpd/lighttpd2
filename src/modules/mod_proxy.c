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
 *     - header mangling (X-Forwarded-For, Connection:)
 *     - handle 1xx responses
 *     - keep-alive connections
 *
 * Author:
 *     Copyright (c) 2009 Stefan Bühler
 */

#include <lighttpd/base.h>
#include <lighttpd/plugin_core.h>


LI_API gboolean mod_proxy_init(liModules *mods, liModule *mod);
LI_API gboolean mod_proxy_free(liModules *mods, liModule *mod);


typedef struct proxy_connection proxy_connection;
typedef struct proxy_context proxy_context;


typedef enum {
	SS_WAIT_FOR_REQUEST,
	SS_CONNECT,
	SS_CONNECTING,
	SS_CONNECTED,
	SS_DONE
} proxy_state;


struct proxy_connection {
	proxy_context *ctx;
	liVRequest *vr;
	proxy_state state;
	int fd;
	ev_io fd_watcher;
	liChunkQueue *proxy_in, *proxy_out;

	liHttpResponseCtx parse_response_ctx;
	gboolean response_headers_finished;
};

struct proxy_context {
	gint refcount;
	liSocketAddress socket;
	GString *socket_str;
	guint timeout;
	liPlugin *plugin;
};

/**********************************************************************************/

static proxy_context* proxy_context_new(liServer *srv, liPlugin *p, GString *dest_socket) {
	liSocketAddress saddr;
	proxy_context* ctx;
	saddr = li_sockaddr_from_string(dest_socket, 80);
	if (NULL == saddr.addr) {
		ERROR(srv, "Invalid socket address '%s'", dest_socket->str);
		return NULL;
	}
	ctx = g_slice_new0(proxy_context);
	ctx->refcount = 1;
	ctx->socket = saddr;
	ctx->timeout = 5;
	ctx->plugin = p;
	ctx->socket_str = g_string_new_len(GSTR_LEN(dest_socket));
	return ctx;
}

static void proxy_context_release(proxy_context *ctx) {
	if (!ctx) return;
	assert(g_atomic_int_get(&ctx->refcount) > 0);
	if (g_atomic_int_dec_and_test(&ctx->refcount)) {
		li_sockaddr_clear(&ctx->socket);
		g_string_free(ctx->socket_str, TRUE);
		g_slice_free(proxy_context, ctx);
	}
}

static void proxy_context_acquire(proxy_context *ctx) {
	assert(g_atomic_int_get(&ctx->refcount) > 0);
	g_atomic_int_inc(&ctx->refcount);
}

static void proxy_fd_cb(struct ev_loop *loop, ev_io *w, int revents);

static proxy_connection* proxy_connection_new(liVRequest *vr, proxy_context *ctx) {
	proxy_connection* pcon = g_slice_new0(proxy_connection);

	proxy_context_acquire(ctx);
	pcon->ctx = ctx;
	pcon->vr = vr;
	pcon->fd = -1;
	ev_init(&pcon->fd_watcher, proxy_fd_cb);
	ev_io_set(&pcon->fd_watcher, -1, 0);
	pcon->fd_watcher.data = pcon;
	pcon->proxy_in = li_chunkqueue_new();
	pcon->proxy_out = li_chunkqueue_new();
	pcon->state = SS_WAIT_FOR_REQUEST;
	li_http_response_parser_init(&pcon->parse_response_ctx, &vr->response, pcon->proxy_in, FALSE, FALSE);
	pcon->response_headers_finished = FALSE;
	return pcon;
}

static void proxy_connection_free(proxy_connection *pcon) {
	liVRequest *vr;
	if (!pcon) return;

	vr = pcon->vr;
	ev_io_stop(vr->wrk->loop, &pcon->fd_watcher);
	proxy_context_release(pcon->ctx);
	if (pcon->fd != -1) close(pcon->fd);
	li_vrequest_backend_finished(vr);

	li_chunkqueue_free(pcon->proxy_in);
	li_chunkqueue_free(pcon->proxy_out);

	li_http_response_parser_clear(&pcon->parse_response_ctx);

	g_slice_free(proxy_connection, pcon);
}

/**********************************************************************************/
/* proxy stream helper */

static void stream_send_chunks(liChunkQueue *out, liChunkQueue *in) {
	li_chunkqueue_steal_all(out, in);

	if (in->is_closed && !out->is_closed) {
		out->is_closed = TRUE;
	}
}

/**********************************************************************************/

static void proxy_send_headers(liVRequest *vr, proxy_connection *pcon) {
	GString *head = g_string_sized_new(4095);
	liHttpHeader *header;
	GList *iter;
	gchar *enc_path;

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
		g_string_append_len(head, CONST_STR_LEN(" HTTP/1.1\r\n"));
		break;
	case LI_HTTP_VERSION_1_0:
	default:
		g_string_append_len(head, CONST_STR_LEN(" HTTP/1.0\r\n"));
		break;
	}

#if 0
	proxy_append_header(con, "X-Forwarded-For", (char *)inet_ntop_cache_get_ip(srv, &(con->dst_addr)));
	/* http_host is NOT is just a pointer to a buffer
	 * which is NULL if it is not set */
	if (con->request.http_host &&
	    !buffer_is_empty(con->request.http_host)) {
		proxy_set_header(con, "X-Host", con->request.http_host->ptr);
	}
	proxy_set_header(con, "X-Forwarded-Proto", con->conf.is_ssl ? "https" : "http");

	/* request header */
	for (i = 0; i < con->request.headers->used; i++) {
		data_string *ds;

		ds = (data_string *)con->request.headers->data[i];

		if (ds->value->used && ds->key->used) {
			if (buffer_is_equal_string(ds->key, CONST_STR_LEN("Connection"))) continue;
			if (buffer_is_equal_string(ds->key, CONST_STR_LEN("Proxy-Connection"))) continue;

			buffer_append_string_buffer(b, ds->key);
			buffer_append_string_len(b, CONST_STR_LEN(": "));
			buffer_append_string_buffer(b, ds->value);
			buffer_append_string_len(b, CONST_STR_LEN("\r\n"));
		}
	}
#endif

	for (iter = g_queue_peek_head_link(&vr->request.headers->entries); iter; iter = g_list_next(iter)) {
		header = (liHttpHeader*) iter->data;
		g_string_append_len(head, GSTR_LEN(header->data));
		g_string_append_len(head, CONST_STR_LEN("\r\n"));
	}
	g_string_append_len(head, CONST_STR_LEN("\r\n"));

	li_chunkqueue_append_string(pcon->proxy_out, head);
}

static void proxy_forward_request(liVRequest *vr, proxy_connection *pcon) {
	stream_send_chunks(pcon->proxy_out, vr->in);
	if (pcon->proxy_out->length > 0)
		li_ev_io_add_events(vr->wrk->loop, &pcon->fd_watcher, EV_WRITE);
}

/**********************************************************************************/

static liHandlerResult proxy_statemachine(liVRequest *vr, proxy_connection *pcon);

static void proxy_fd_cb(struct ev_loop *loop, ev_io *w, int revents) {
	proxy_connection *pcon = (proxy_connection*) w->data;

	if (pcon->state == SS_CONNECTING) {
		if (LI_HANDLER_GO_ON != proxy_statemachine(pcon->vr, pcon)) {
			li_vrequest_error(pcon->vr);
		}
		return;
	}

	if (revents & EV_READ) {
		if (pcon->proxy_in->is_closed) {
			li_ev_io_rem_events(loop, w, EV_READ);
		} else {
			switch (li_network_read(pcon->vr, w->fd, pcon->proxy_in)) {
			case LI_NETWORK_STATUS_SUCCESS:
				break;
			case LI_NETWORK_STATUS_FATAL_ERROR:
				VR_ERROR(pcon->vr, "(%s) network read fatal error", pcon->ctx->socket_str->str);
				li_vrequest_error(pcon->vr);
				return;
			case LI_NETWORK_STATUS_CONNECTION_CLOSE:
				pcon->proxy_in->is_closed = TRUE;
				ev_io_stop(loop, w);
				close(pcon->fd);
				pcon->fd = -1;
				li_vrequest_backend_finished(pcon->vr);
				break;
			case LI_NETWORK_STATUS_WAIT_FOR_EVENT:
				break;
			case LI_NETWORK_STATUS_WAIT_FOR_AIO_EVENT:
				/* TODO: aio */
				li_ev_io_rem_events(loop, w, EV_READ);
				break;
			}
		}
	}

	if (pcon->fd != -1 && (revents & EV_WRITE)) {
		if (pcon->proxy_out->length > 0) {
			switch (li_network_write(pcon->vr, w->fd, pcon->proxy_out, 256*1024)) {
			case LI_NETWORK_STATUS_SUCCESS:
				break;
			case LI_NETWORK_STATUS_FATAL_ERROR:
				VR_ERROR(pcon->vr, "(%s) network write fatal error", pcon->ctx->socket_str->str);
				li_vrequest_error(pcon->vr);
				return;
			case LI_NETWORK_STATUS_CONNECTION_CLOSE:
				pcon->proxy_in->is_closed = TRUE;
				ev_io_stop(loop, w);
				close(pcon->fd);
				pcon->fd = -1;
				li_vrequest_backend_finished(pcon->vr);
				break;
			case LI_NETWORK_STATUS_WAIT_FOR_EVENT:
				break;
			case LI_NETWORK_STATUS_WAIT_FOR_AIO_EVENT:
				li_ev_io_rem_events(loop, w, EV_WRITE);
				/* TODO: aio */
				break;
			}
		}
		if (pcon->proxy_out->length == 0) {
			li_ev_io_rem_events(loop, w, EV_WRITE);
		}
	}

	if (!pcon->response_headers_finished && LI_HANDLER_GO_ON == li_http_response_parse(pcon->vr, &pcon->parse_response_ctx)) {
		pcon->response_headers_finished = TRUE;
		li_vrequest_handle_response_headers(pcon->vr);
	}

	if (pcon->response_headers_finished) {
		li_chunkqueue_steal_all(pcon->vr->out, pcon->proxy_in);
		pcon->vr->out->is_closed = pcon->proxy_in->is_closed;
		li_vrequest_handle_response_body(pcon->vr);
	}

	/* only possible if we didn't found a header */
	if (pcon->proxy_in->is_closed && !pcon->vr->out->is_closed) {
		VR_ERROR(pcon->vr, "(%s) unexpected end-of-file (perhaps the proxy process died)", pcon->ctx->socket_str->str);
		li_vrequest_error(pcon->vr);
	}
}

/**********************************************************************************/
/* state machine */

static void proxy_close(liVRequest *vr, liPlugin *p);

static liHandlerResult proxy_statemachine(liVRequest *vr, proxy_connection *pcon) {
	liPlugin *p = pcon->ctx->plugin;

	switch (pcon->state) {
	case SS_WAIT_FOR_REQUEST:
		/* do *not* wait until we have all data */
		pcon->state = SS_CONNECT;

		/* fall through */
	case SS_CONNECT:
		do {
			pcon->fd = socket(pcon->ctx->socket.addr->plain.sa_family, SOCK_STREAM, 0);
		} while (-1 == pcon->fd && errno == EINTR);
		if (-1 == pcon->fd) {
			if (errno == EMFILE) {
				li_server_out_of_fds(vr->wrk->srv);
			}
			VR_ERROR(vr, "Couldn't open socket: %s", g_strerror(errno));
			return LI_HANDLER_ERROR;
		}
		li_fd_init(pcon->fd);
		ev_io_set(&pcon->fd_watcher, pcon->fd, EV_READ | EV_WRITE);
		ev_io_start(vr->wrk->loop, &pcon->fd_watcher);

		/* fall through */
	case SS_CONNECTING:
		if (-1 == connect(pcon->fd, &pcon->ctx->socket.addr->plain, pcon->ctx->socket.len)) {
			switch (errno) {
			case EINPROGRESS:
			case EALREADY:
			case EINTR:
				pcon->state = SS_CONNECTING;
				return LI_HANDLER_GO_ON;
			case EAGAIN: /* backend overloaded */
				proxy_close(vr, p);
				li_vrequest_backend_overloaded(vr);
				return LI_HANDLER_GO_ON;
			default:
				VR_ERROR(vr, "Couldn't connect to '%s': %s",
					li_sockaddr_to_string(pcon->ctx->socket, vr->wrk->tmp_str, TRUE)->str,
					g_strerror(errno));
				proxy_close(vr, p);
				li_vrequest_backend_dead(vr);
				return LI_HANDLER_GO_ON;
			}
		}

		pcon->state = SS_CONNECTED;

		/* prepare stream */
		proxy_send_headers(vr, pcon);

		/* fall through */
	case SS_CONNECTED:
		proxy_forward_request(vr, pcon);
		break;

	case SS_DONE:
		break;
	}

	return LI_HANDLER_GO_ON;
}


/**********************************************************************************/

static liHandlerResult proxy_handle(liVRequest *vr, gpointer param, gpointer *context) {
	proxy_context *ctx = (proxy_context*) param;
	proxy_connection *pcon;
	UNUSED(context);
	if (!li_vrequest_handle_indirect(vr, ctx->plugin)) return LI_HANDLER_GO_ON;

	pcon = proxy_connection_new(vr, ctx);
	if (!pcon) {
		return LI_HANDLER_ERROR;
	}
	g_ptr_array_index(vr->plugin_ctx, ctx->plugin->id) = pcon;

	li_chunkqueue_set_limit(pcon->proxy_in, vr->out->limit);
	li_chunkqueue_set_limit(pcon->proxy_out, vr->in->limit);
	if (vr->out->limit) vr->out->limit->io_watcher = &pcon->fd_watcher;

	return proxy_statemachine(vr, pcon);
}


static liHandlerResult proxy_handle_request_body(liVRequest *vr, liPlugin *p) {
	proxy_connection *pcon = (proxy_connection*) g_ptr_array_index(vr->plugin_ctx, p->id);
	if (!pcon) return LI_HANDLER_ERROR;

	return proxy_statemachine(vr, pcon);
}

static void proxy_close(liVRequest *vr, liPlugin *p) {
	proxy_connection *pcon = (proxy_connection*) g_ptr_array_index(vr->plugin_ctx, p->id);
	g_ptr_array_index(vr->plugin_ctx, p->id) = NULL;
	if (pcon) {
		if (vr->out->limit) vr->out->limit->io_watcher = NULL;
		proxy_connection_free(pcon);
	}
}


static void proxy_free(liServer *srv, gpointer param) {
	proxy_context *ctx = (proxy_context*) param;
	UNUSED(srv);

	proxy_context_release(ctx);
}

static liAction* proxy_create(liServer *srv, liPlugin* p, liValue *val, gpointer userdata) {
	proxy_context *ctx;
	UNUSED(userdata);

	if (val->type != LI_VALUE_STRING) {
		ERROR(srv, "%s", "proxy expects a string as parameter");
		return FALSE;
	}

	ctx = proxy_context_new(srv, p, val->data.string);
	if (!ctx) return NULL;

	return li_action_new_function(proxy_handle, NULL, proxy_free, ctx);
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

	p->handle_request_body = proxy_handle_request_body;
	p->handle_vrclose = proxy_close;
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

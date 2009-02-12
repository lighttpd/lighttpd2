/*
 * mod_fastcgi - connect to fastcgi backends for generating content
 *
 * Description:
 *     mod_fastcgi connects to a backend over tcp or unix sockets
 *
 * Setups:
 *     none
 * Options:
 *     fastcgi.log_plain_errors <value> - whether to prepend timestamp and other info to
 *                                        fastcgi stderr lines in the "backend" log.
 *         type: boolean
 * Actions:
 *     fastcgi <socket>  - connect to backend at <socket>
 *         socket: string, either "ip:port" or "unix:/path"
 *
 * Example config:
 *     fastcgi "127.0.0.1:9090"
 *
 * Todo:
 *     - reuse fastcgi connections (keepalive)
 *     - send more infos to backend (http headers, auth info)
 *     - option for alternative doc-root?
 *
 * Author:
 *     Copyright (c) 2009 Stefan Bühler
 */

#include <lighttpd/base.h>
#include <lighttpd/plugin_core.h>

enum fastcgi_options_t {
	FASTCGI_OPTION_LOG_PLAIN_ERRORS = 0,
};

#define FASTCGI_OPTION(idx) _FASTCGI_OPTION(vr, idx)
#define _FASTCGI_OPTION(vr, idx) _OPTION_ABS(vr, p->opt_base_index + idx)


LI_API gboolean mod_fastcgi_init(modules *mods, module *mod);
LI_API gboolean mod_fastcgi_free(modules *mods, module *mod);


struct fastcgi_connection;
typedef struct fastcgi_connection fastcgi_connection;
struct fastcgi_context;
typedef struct fastcgi_context fastcgi_context;
struct FCGI_Record;
typedef struct FCGI_Record FCGI_Record;


typedef enum {
	FS_WAIT_FOR_REQUEST,
	FS_CONNECT,
	FS_CONNECTING,
	FS_CONNECTED,
	FS_DONE
} fastcgi_state;


struct FCGI_Record {
	guint8 version;
	guint8 type;
	guint16 requestID;
	guint16 contentLength;
	guint8 paddingLength;
};


struct fastcgi_connection {
	fastcgi_context *ctx;
	vrequest *vr;
	fastcgi_state state;
	int fd;
	ev_io fd_watcher;
	chunkqueue *fcgi_in, *fcgi_out, *stdout;

	GString *buf_in_record;
	FCGI_Record fcgi_in_record;
	guint16 requestid;
	
	http_response_ctx parse_response_ctx;
	gboolean response_headers_finished;
};

struct fastcgi_context {
	gint refcount;
	sockaddr socket;
	guint timeout;
	plugin *plugin;
};

/* fastcgi types */

#define FCGI_VERSION_1           1
#define FCGI_HEADER_LEN  8

enum FCGI_Type {
	FCGI_BEGIN_REQUEST     = 1,
	FCGI_ABORT_REQUEST     = 2,
	FCGI_END_REQUEST       = 3,
	FCGI_PARAMS            = 4,
	FCGI_STDIN             = 5,
	FCGI_STDOUT            = 6,
	FCGI_STDERR            = 7,
	FCGI_DATA              = 8,
	FCGI_GET_VALUES        = 9,
	FCGI_GET_VALUES_RESULT = 10,
	FCGI_UNKNOWN_TYPE      = 11
};
#define FCGI_MAXTYPE (FCGI_UNKNOWN_TYPE)

enum FCGI_Flags {
	FCGI_KEEP_CONN  = 1
};

enum FCGI_Role {
	FCGI_RESPONDER  = 1,
	FCGI_AUTHORIZER = 2,
	FCGI_FILTER     = 3
};

enum FCGI_ProtocolStatus {
	FCGI_REQUEST_COMPLETE = 0,
	FCGI_CANT_MPX_CONN    = 1,
	FCGI_OVERLOADED       = 2,
	FCGI_UNKNOWN_ROLE     = 3
};

/**********************************************************************************/

static fastcgi_context* fastcgi_context_new(server *srv, plugin *p, GString *dest_socket) {
	sockaddr saddr;
	fastcgi_context* ctx;
	saddr = sockaddr_from_string(dest_socket, 0);
	if (NULL == saddr.addr) {
		ERROR(srv, "Invalid socket address '%s'", dest_socket->str);
		return NULL;
	}
	ctx = g_slice_new0(fastcgi_context);
	ctx->refcount = 1;
	ctx->socket = saddr;
	ctx->timeout = 5;
	ctx->plugin = p;
	return ctx;
}

static void fastcgi_context_release(fastcgi_context *ctx) {
	if (!ctx) return;
	assert(g_atomic_int_get(&ctx->refcount) > 0);
	if (g_atomic_int_dec_and_test(&ctx->refcount)) {
		sockaddr_clear(&ctx->socket);
		g_slice_free(fastcgi_context, ctx);
	}
}

static void fastcgi_context_acquire(fastcgi_context *ctx) {
	assert(g_atomic_int_get(&ctx->refcount) > 0);
	g_atomic_int_inc(&ctx->refcount);
}

static void fastcgi_fd_cb(struct ev_loop *loop, ev_io *w, int revents);

static fastcgi_connection* fastcgi_connection_new(vrequest *vr, fastcgi_context *ctx) {
	fastcgi_connection* fcon = g_slice_new(fastcgi_connection);

	fastcgi_context_acquire(ctx);
	fcon->ctx = ctx;
	fcon->vr = vr;
	fcon->fd = -1;
	ev_init(&fcon->fd_watcher, fastcgi_fd_cb);
	ev_io_set(&fcon->fd_watcher, -1, 0);
	fcon->fd_watcher.data = fcon;
	fcon->fcgi_in = chunkqueue_new();
	fcon->fcgi_out = chunkqueue_new();
	fcon->stdout = chunkqueue_new();
	fcon->buf_in_record = g_string_sized_new(FCGI_HEADER_LEN);
	fcon->requestid = 1;
	fcon->state = FS_WAIT_FOR_REQUEST;
	http_response_parser_init(&fcon->parse_response_ctx, &vr->response, fcon->stdout, TRUE, FALSE);
	fcon->response_headers_finished = FALSE;
	return fcon;
}

static void fastcgi_connection_free(fastcgi_connection *fcon) {
	vrequest *vr;
	if (!fcon) return;

	vr = fcon->vr;
	ev_io_stop(vr->con->wrk->loop, &fcon->fd_watcher);
	fastcgi_context_release(fcon->ctx);
	if (fcon->fd != -1) close(fcon->fd);

	chunkqueue_free(fcon->fcgi_in);
	chunkqueue_free(fcon->fcgi_out);
	chunkqueue_free(fcon->stdout);
	g_string_free(fcon->buf_in_record, TRUE);

	http_response_parser_clear(&fcon->parse_response_ctx);

	g_slice_free(fastcgi_connection, fcon);
}

/**********************************************************************************/
/* fastcgi stream helper */

static const gchar __padding[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };

static void append_padding(GString *s, guint8 padlen) {
	g_string_append_len(s, __padding, padlen);
}

/* returns padding length */
static guint8 stream_build_fcgi_record(GString *buf, guint8 type, guint16 requestid, guint16 datalen) {
	guint16 w;
	guint8 padlen = (8 - (datalen & 0x7)) % 8; /* padding must be < 8 */

	g_string_set_size(buf, FCGI_HEADER_LEN);
	g_string_truncate(buf, 0);

	g_string_append_c(buf, FCGI_VERSION_1);
	g_string_append_c(buf, type);
	w = htons(requestid);
	g_string_append_len(buf, (const gchar*) &w, sizeof(w));
	w = htons(datalen);
	g_string_append_len(buf, (const gchar*) &w, sizeof(w));
	g_string_append_c(buf, padlen);
	g_string_append_c(buf, 0);
	return padlen;
}

/* returns padding length */
static guint8 stream_send_fcgi_record(chunkqueue *out, guint8 type, guint16 requestid, guint16 datalen) {
	GString *record = g_string_sized_new(FCGI_HEADER_LEN);
	guint8 padlen = stream_build_fcgi_record(record, type, requestid, datalen);
	chunkqueue_append_string(out, record);
	return padlen;
}

static void stream_send_data(chunkqueue *out, guint8 type, guint16 requestid, const gchar *data, size_t datalen) {
	while (datalen > 0) {
		guint16 tosend = (datalen > G_MAXUINT16) ? G_MAXUINT16 : datalen;
		guint8 padlen = stream_send_fcgi_record(out, type, requestid, tosend);
		GString *tmps = g_string_sized_new(tosend + padlen);
		g_string_append_len(tmps, data, tosend);
		append_padding(tmps, padlen);
		chunkqueue_append_string(out, tmps);
		data += tosend;
		datalen -= tosend;
	}
}

/* kills string */
static void stream_send_string(chunkqueue *out, guint8 type, guint16 requestid, GString *data) {
	if (data->len > G_MAXUINT16) {
		stream_send_data(out, type, requestid, GSTR_LEN(data));
		g_string_free(data, TRUE);
	} else {
		guint8 padlen = stream_send_fcgi_record(out, type, requestid, data->len);
		append_padding(data, padlen);
		chunkqueue_append_string(out, data);
	}
}

static void stream_send_chunks(chunkqueue *out, guint8 type, guint16 requestid, chunkqueue *in) {
	while (in->length > 0) {
		guint16 tosend = (in->length > G_MAXUINT16) ? G_MAXUINT16 : in->length;
		guint8 padlen = stream_send_fcgi_record(out, type, requestid, tosend);
		chunkqueue_steal_len(out, in, tosend);
		chunkqueue_append_mem(out, __padding, padlen);
	}

	if (in->is_closed && !out->is_closed) {
		out->is_closed = TRUE;
		stream_send_fcgi_record(out, type, requestid, 0);
	}
}

static gboolean _append_str_len(GString *s, size_t len) {
	if (len > G_MAXINT32) return FALSE;
	if (len > 127) {
		guint32 i = htonl(len | (1 << 31));
		g_string_append_len(s, (const gchar*) &i, sizeof(i));
	} else {
		g_string_append_c(s, (char) len);
	}
	return TRUE;
}

static gboolean append_key_value_pair(GString *s, const gchar *key, size_t keylen, const gchar *val, size_t valuelen) {
	if (!_append_str_len(s, keylen) || !_append_str_len(s, valuelen)) return FALSE;
	g_string_append_len(s, key, keylen);
	g_string_append_len(s, val, valuelen);
	return TRUE;
}

/**********************************************************************************/

static void fastcgi_send_begin(fastcgi_connection *fcon) {
	GString *buf = g_string_sized_new(16);
	guint16 w;

	stream_build_fcgi_record(buf, FCGI_BEGIN_REQUEST, fcon->requestid, 8);
	w = htons(FCGI_RESPONDER);
	g_string_append_len(buf, (const char*) &w, sizeof(w));
	g_string_append_c(buf, 0); /* TODO: FCGI_KEEP_CONN */
	append_padding(buf, 5);
	chunkqueue_append_string(fcon->fcgi_out, buf);
}

static void fastcgi_env_setup(vrequest *vr) {
	connection *con = vr->con;
	GString *tmp = con->wrk->tmp_str;
	environment_insert(&vr->env, CONST_STR_LEN("SERVER_SOFTWARE"), GSTR_LEN(CORE_OPTION(CORE_OPTION_SERVER_TAG).string));
	environment_insert(&vr->env, CONST_STR_LEN("SERVER_NAME"), GSTR_LEN(vr->request.uri.host));
	environment_insert(&vr->env, CONST_STR_LEN("GATEWAY_INTERFACE"), CONST_STR_LEN("CGI/1.1"));
	{
		guint port = 0;
		switch (con->local_addr.plain.sa_family) {
		case AF_INET: port = con->local_addr.ipv4.sin_port; break;
#ifdef HAVE_IPV6
		case AF_INET6: port = con->local_addr.ipv6.sin6_port; break;
#endif
		}
		if (port) {
			g_string_printf(tmp, "%u", port);
			environment_insert(&vr->env, CONST_STR_LEN("SERVER_PORT"), GSTR_LEN(tmp));
		}
	}
	{
		sockaddr_to_string(&con->local_addr, tmp);
		environment_insert(&vr->env, CONST_STR_LEN("SERVER_ADDR"), GSTR_LEN(tmp));
	}

	{
		guint port = 0;
		switch (con->remote_addr.plain.sa_family) {
		case AF_INET: port = con->remote_addr.ipv4.sin_port; break;
#ifdef HAVE_IPV6
		case AF_INET6: port = con->remote_addr.ipv6.sin6_port; break;
#endif
		}
		if (port) {
			g_string_printf(tmp, "%u", port);
			environment_insert(&vr->env, CONST_STR_LEN("REMOTE_PORT"), GSTR_LEN(tmp));
		}
	}
	{
		sockaddr_to_string(&con->remote_addr, tmp);
		environment_insert(&vr->env, CONST_STR_LEN("REMOTE_ADDR"), GSTR_LEN(tmp));
	}

	/* TODO? auth vars; i think it would be easier if the auth mod sets them:
	 * REMOTE_USER, AUTH_TYPE
	 */
	{
		g_string_printf(tmp, "%" L_GOFFSET_MODIFIER "i", vr->request.content_length);
		environment_insert(&vr->env, CONST_STR_LEN("CONTENT_LENGTH"), GSTR_LEN(tmp));
	}

	environment_insert(&vr->env, CONST_STR_LEN("SCRIPT_NAME"), GSTR_LEN(vr->request.uri.path));

	environment_insert(&vr->env, CONST_STR_LEN("PATH_INFO"), GSTR_LEN(vr->physical.pathinfo));
	if (vr->physical.pathinfo->len) {
		g_string_truncate(tmp, 0);
		g_string_append_len(tmp, GSTR_LEN(vr->physical.doc_root)); /* TODO: perhaps an option for alternative doc-root? */
		g_string_append_len(tmp, GSTR_LEN(vr->physical.pathinfo));
		environment_insert(&vr->env, CONST_STR_LEN("PATH_TRANSLATED"), GSTR_LEN(tmp));
	}

	environment_insert(&vr->env, CONST_STR_LEN("SCRIPT_FILENAME"), GSTR_LEN(vr->physical.path));
	environment_insert(&vr->env, CONST_STR_LEN("DOCUMENT_ROOT"), GSTR_LEN(vr->physical.doc_root));

	environment_insert(&vr->env, CONST_STR_LEN("REQUEST_URI"), GSTR_LEN(vr->request.uri.orig_path));
	if (!g_string_equal(vr->request.uri.orig_path, vr->request.uri.path)) {
		environment_insert(&vr->env, CONST_STR_LEN("REDIRECT_URI"), GSTR_LEN(vr->request.uri.path));
	}
	environment_insert(&vr->env, CONST_STR_LEN("QUERY_STRING"), GSTR_LEN(vr->request.uri.query));

	environment_insert(&vr->env, CONST_STR_LEN("REQUEST_METHOD"), GSTR_LEN(vr->request.http_method_str));
	environment_insert(&vr->env, CONST_STR_LEN("REDIRECT_STATUS"), CONST_STR_LEN("200")); /* if php is compiled with --force-redirect */
	switch (vr->request.http_version) {
	case HTTP_VERSION_1_1:
		environment_insert(&vr->env, CONST_STR_LEN("SERVER_PROTOCOL"), CONST_STR_LEN("HTTP/1.1"));
		break;
	case HTTP_VERSION_1_0:
	default:
		environment_insert(&vr->env, CONST_STR_LEN("SERVER_PROTOCOL"), CONST_STR_LEN("HTTP/1.0"));
		break;
	}

	if (con->is_ssl) {
		environment_insert(&vr->env, CONST_STR_LEN("HTTPS"), CONST_STR_LEN("on"));
	}
}

static void fastcgi_send_env(vrequest *vr, fastcgi_connection *fcon) {
	GHashTableIter i;
	gpointer key, val;
	GString *buf = g_string_sized_new(0);

	g_hash_table_iter_init(&i, vr->env.table);
	while (g_hash_table_iter_next(&i, &key, &val)) {
		append_key_value_pair(buf, GSTR_LEN((GString*) key), GSTR_LEN((GString*) val));
	}

	/* TODO: send headers */

	stream_send_string(fcon->fcgi_out, FCGI_PARAMS, fcon->requestid, buf);
	stream_send_fcgi_record(fcon->fcgi_out, FCGI_PARAMS, fcon->requestid, 0);
}

static void fastcgi_forward_request(vrequest *vr, fastcgi_connection *fcon) {
	stream_send_chunks(fcon->fcgi_out, FCGI_STDIN, fcon->requestid, vr->in);
	if (fcon->fcgi_out->length > 0)
		ev_io_add_events(vr->con->wrk->loop, &fcon->fd_watcher, EV_WRITE);
}

static gboolean fastcgi_get_packet(fastcgi_connection *fcon) {
	const unsigned char *data;
	gint len;
	if (!chunkqueue_extract_to(fcon->vr, fcon->fcgi_in, FCGI_HEADER_LEN, fcon->buf_in_record)) return FALSE; /* need more data */

	data = fcon->buf_in_record->str;
	fcon->fcgi_in_record.version = data[0];
	fcon->fcgi_in_record.type = data[1];
	fcon->fcgi_in_record.requestID = (data[2] << 8) | (data[3]);
	fcon->fcgi_in_record.contentLength = (data[4] << 8) | (data[5]);
	fcon->fcgi_in_record.paddingLength = data[6];

	len = ((gint) fcon->fcgi_in_record.contentLength) + fcon->fcgi_in_record.paddingLength + FCGI_HEADER_LEN;

	if (len > fcon->fcgi_in->length) return FALSE; /* need more data */

	return TRUE;
}

static gboolean fastcgi_parse_response(fastcgi_connection *fcon) {
	vrequest *vr = fcon->vr;
	plugin *p = fcon->ctx->plugin;
	while (fastcgi_get_packet(fcon)) {
		if (fcon->fcgi_in_record.version != FCGI_VERSION_1) {
			VR_ERROR(vr, "Unknown fastcgi protocol version %i", (gint) fcon->fcgi_in_record.version);
			close(fcon->fd);
			fcon->fd = -1;
			vrequest_error(vr);
			return FALSE;
		}
		chunkqueue_skip(fcon->fcgi_in, FCGI_HEADER_LEN);
		switch (fcon->fcgi_in_record.type) {
		case FCGI_END_REQUEST:
			chunkqueue_skip(fcon->fcgi_in, fcon->fcgi_in_record.contentLength);
			fcon->stdout->is_closed = TRUE;
			break;
		case FCGI_STDOUT:
			if (0 == fcon->fcgi_in_record.contentLength) {
				fcon->stdout->is_closed = TRUE;
			} else {
				chunkqueue_steal_len(fcon->stdout, fcon->fcgi_in, fcon->fcgi_in_record.contentLength);
			}
			break;
		case FCGI_STDERR:
			chunkqueue_extract_to(vr, fcon->fcgi_in, fcon->fcgi_in_record.contentLength, vr->con->wrk->tmp_str);
			if (FASTCGI_OPTION(FASTCGI_OPTION_LOG_PLAIN_ERRORS).boolean) {
				log_split_lines(vr->con->srv, vr, LOG_LEVEL_BACKEND, 0, vr->con->wrk->tmp_str->str, "");
			} else {
				VR_BACKEND_LINES(vr, vr->con->wrk->tmp_str->str, "%s", "(fcgi-stderr) ");
			}
			chunkqueue_skip(fcon->fcgi_in, fcon->fcgi_in_record.contentLength);
			break;
		default:
			VR_WARNING(vr, "Unhandled fastcgi record type %i", (gint) fcon->fcgi_in_record.type);
			chunkqueue_skip(fcon->fcgi_in, fcon->fcgi_in_record.contentLength);
			break;
		}
		chunkqueue_skip(fcon->fcgi_in, fcon->fcgi_in_record.paddingLength);
	}
	return TRUE;
}

/**********************************************************************************/

static handler_t fastcgi_statemachine(vrequest *vr, fastcgi_connection *fcon);

static void fastcgi_fd_cb(struct ev_loop *loop, ev_io *w, int revents) {
	fastcgi_connection *fcon = (fastcgi_connection*) w->data;

	if (fcon->state == FS_CONNECTING) {
		if (HANDLER_GO_ON != fastcgi_statemachine(fcon->vr, fcon)) {
			vrequest_error(fcon->vr);
		}
		return;
	}

	if (revents & EV_READ) {
		if (fcon->fcgi_in->is_closed) {
			ev_io_rem_events(loop, w, EV_READ);
		} else {
			switch (network_read(fcon->vr, w->fd, fcon->fcgi_in)) {
			case NETWORK_STATUS_SUCCESS:
				break;
			case NETWORK_STATUS_FATAL_ERROR:
				VR_ERROR(fcon->vr, "%s", "network read fatal error");
				vrequest_error(fcon->vr);
				return;
			case NETWORK_STATUS_CONNECTION_CLOSE:
				fcon->fcgi_in->is_closed = TRUE;
				ev_io_stop(loop, w);
				close(fcon->fd);
				fcon->fd = -1;
				break;
			case NETWORK_STATUS_WAIT_FOR_EVENT:
				break;
			case NETWORK_STATUS_WAIT_FOR_AIO_EVENT:
				/* TODO: aio */
				ev_io_rem_events(loop, w, EV_READ);
				break;
			}
		}
	}

	if (fcon->fd != -1 && (revents & EV_WRITE)) {
		if (fcon->fcgi_out->length > 0) {
			switch (network_write(fcon->vr, w->fd, fcon->fcgi_out)) {
			case NETWORK_STATUS_SUCCESS:
				break;
			case NETWORK_STATUS_FATAL_ERROR:
				VR_ERROR(fcon->vr, "%s", "network write fatal error");
				vrequest_error(fcon->vr);
				return;
			case NETWORK_STATUS_CONNECTION_CLOSE:
				fcon->fcgi_in->is_closed = TRUE;
				ev_io_stop(loop, w);
				close(fcon->fd);
				fcon->fd = -1;
				break;
			case NETWORK_STATUS_WAIT_FOR_EVENT:
				break;
			case NETWORK_STATUS_WAIT_FOR_AIO_EVENT:
				ev_io_rem_events(loop, w, EV_WRITE);
				/* TODO: aio */
				break;
			}
		}
		if (fcon->fcgi_out->length == 0) {
			ev_io_rem_events(loop, w, EV_WRITE);
		}
	}

	if (!fastcgi_parse_response(fcon)) return;

	if (!fcon->response_headers_finished && HANDLER_GO_ON == http_response_parse(fcon->vr, &fcon->parse_response_ctx)) {
		fcon->response_headers_finished = TRUE;
		vrequest_handle_response_headers(fcon->vr);
	}
	
	if (fcon->response_headers_finished) {
		chunkqueue_steal_all(fcon->vr->out, fcon->stdout);
		fcon->vr->out->is_closed = fcon->stdout->is_closed;
		vrequest_handle_response_body(fcon->vr);
	}

	if (fcon->fcgi_in->is_closed && !fcon->vr->out->is_closed) {
		VR_ERROR(fcon->vr, "%s", "unexpected end-of-file (perhaps the fastcgi process died)");
		vrequest_error(fcon->vr);
	}
}

/**********************************************************************************/
/* state machine */

static void fastcgi_close(vrequest *vr, plugin *p);

static handler_t fastcgi_statemachine(vrequest *vr, fastcgi_connection *fcon) {
	plugin *p = fcon->ctx->plugin;

	switch (fcon->state) {
	case FS_WAIT_FOR_REQUEST:
		if (-1 == vr->request.content_length || vr->request.content_length != vr->in->length) return HANDLER_GO_ON;
		fcon->state = FS_CONNECT;

		/* fall through */
	case FS_CONNECT:
		do {
			fcon->fd = socket(fcon->ctx->socket.addr->plain.sa_family, SOCK_STREAM, 0);
		} while (-1 == fcon->fd && errno == EINTR);
		if (-1 == fcon->fd) {
			if (errno == EMFILE) {
				server_out_of_fds(vr->con->srv);
			}
			VR_ERROR(vr, "Couldn't open socket: %s", g_strerror(errno));
			return HANDLER_ERROR;
		}
		fd_init(fcon->fd);
		ev_io_set(&fcon->fd_watcher, fcon->fd, EV_READ | EV_WRITE);
		ev_io_start(vr->con->wrk->loop, &fcon->fd_watcher);

		/* fall through */
	case FS_CONNECTING:
		if (-1 == connect(fcon->fd, &fcon->ctx->socket.addr->plain, fcon->ctx->socket.len)) {
			switch (errno) {
			case EINPROGRESS:
			case EALREADY:
			case EINTR:
				fcon->state = FS_CONNECTING;
				return HANDLER_GO_ON;
			case EAGAIN: /* backend overloaded */
				fastcgi_close(vr, p);
				vrequest_backend_overloaded(vr);
				return HANDLER_GO_ON;
			default:
				VR_ERROR(vr, "Couldn't connect: %s", g_strerror(errno));
				fastcgi_close(vr, p);
				vrequest_backend_dead(vr);
				return HANDLER_GO_ON;
			}
		}

		fcon->state = FS_CONNECTED;

		/* prepare stream */
		fastcgi_send_begin(fcon);
		fastcgi_env_setup(vr);
		fastcgi_send_env(vr, fcon);

		/* fall through */
	case FS_CONNECTED:
		fastcgi_forward_request(vr, fcon);
		break;

	case FS_DONE:
		break;
	}

	return HANDLER_GO_ON;
}


/**********************************************************************************/

static handler_t fastcgi_handle(vrequest *vr, gpointer param, gpointer *context) {
	fastcgi_context *ctx = (fastcgi_context*) param;
	fastcgi_connection *fcon;
	UNUSED(context);
	if (!vrequest_handle_indirect(vr, ctx->plugin)) return HANDLER_GO_ON;

	fcon = fastcgi_connection_new(vr, ctx);
	if (!fcon) {
		return HANDLER_ERROR;
	}
	g_ptr_array_index(vr->plugin_ctx, ctx->plugin->id) = fcon;

	return fastcgi_statemachine(vr, fcon);
}


static handler_t fastcgi_handle_request_body(vrequest *vr, plugin *p) {
	fastcgi_connection *fcon = (fastcgi_connection*) g_ptr_array_index(vr->plugin_ctx, p->id);
	if (!fcon) return HANDLER_ERROR;

	return fastcgi_statemachine(vr, fcon);
}

static void fastcgi_close(vrequest *vr, plugin *p) {
	fastcgi_connection *fcon = (fastcgi_connection*) g_ptr_array_index(vr->plugin_ctx, p->id);
	g_ptr_array_index(vr->plugin_ctx, p->id) = NULL;

	fastcgi_connection_free(fcon);
}


static void fastcgi_free(server *srv, gpointer param) {
	fastcgi_context *ctx = (fastcgi_context*) param;
	UNUSED(srv);

	fastcgi_context_release(ctx);
}

static action* fastcgi_create(server *srv, plugin* p, value *val) {
	fastcgi_context *ctx;

	if (val->type != VALUE_STRING) {
		ERROR(srv, "%s", "fastcgi expects a string as parameter");
		return FALSE;
	}

	ctx = fastcgi_context_new(srv, p, val->data.string);
	if (!ctx) return NULL;

	return action_new_function(fastcgi_handle, NULL, fastcgi_free, ctx);
}

static const plugin_option options[] = {
	{ "fastcgi.log_plain_errors", VALUE_BOOLEAN, GINT_TO_POINTER(FALSE), NULL, NULL },

	{ NULL, 0, NULL, NULL, NULL }
};

static const plugin_action actions[] = {
	{ "fastcgi", fastcgi_create },
	{ NULL, NULL }
};

static const plugin_setup setups[] = {
	{ NULL, NULL }
};


static void plugin_init(server *srv, plugin *p) {
	UNUSED(srv);

	p->options = options;
	p->actions = actions;
	p->setups = setups;

	p->handle_request_body = fastcgi_handle_request_body;
	p->handle_vrclose = fastcgi_close;
}


gboolean mod_fastcgi_init(modules *mods, module *mod) {
	MODULE_VERSION_CHECK(mods);

	mod->config = plugin_register(mods->main, "mod_fastcgi", plugin_init);

	return mod->config != NULL;
}

gboolean mod_fastcgi_free(modules *mods, module *mod) {
	if (mod->config)
		plugin_free(mods->main, mod->config);

	return TRUE;
}

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

LI_API gboolean mod_fastcgi_init(liModules *mods, liModule *mod);
LI_API gboolean mod_fastcgi_free(liModules *mods, liModule *mod);


typedef struct fastcgi_connection fastcgi_connection;
typedef struct fastcgi_context fastcgi_context;
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
	gint remainingContent, remainingPadding;
	gboolean valid, first;
};


struct fastcgi_connection {
	fastcgi_context *ctx;
	liVRequest *vr;
	fastcgi_state state;
	int fd;
	ev_io fd_watcher;
	liChunkQueue *fcgi_in, *fcgi_out, *stdout;
	liBuffer *fcgi_in_buffer;

	GByteArray *buf_in_record;
	FCGI_Record fcgi_in_record;
	guint16 requestid;
	
	liHttpResponseCtx parse_response_ctx;
	gboolean response_headers_finished;
};

struct fastcgi_context {
	gint refcount;
	liSocketAddress socket;
	GString *socket_str;
	guint timeout;
	liPlugin *plugin;

	gint last_errno;
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

static fastcgi_context* fastcgi_context_new(liServer *srv, liPlugin *p, GString *dest_socket) {
	liSocketAddress saddr;
	fastcgi_context* ctx;
	saddr = li_sockaddr_from_string(dest_socket, 0);
	if (NULL == saddr.addr) {
		ERROR(srv, "Invalid socket address '%s'", dest_socket->str);
		return NULL;
	}
	ctx = g_slice_new0(fastcgi_context);
	ctx->refcount = 1;
	ctx->socket = saddr;
	ctx->timeout = 5;
	ctx->plugin = p;
	ctx->socket_str = g_string_new_len(GSTR_LEN(dest_socket));
	return ctx;
}

static void fastcgi_context_release(fastcgi_context *ctx) {
	if (!ctx) return;
	assert(g_atomic_int_get(&ctx->refcount) > 0);
	if (g_atomic_int_dec_and_test(&ctx->refcount)) {
		li_sockaddr_clear(&ctx->socket);
		g_string_free(ctx->socket_str, TRUE);
		g_slice_free(fastcgi_context, ctx);
	}
}

static void fastcgi_context_acquire(fastcgi_context *ctx) {
	assert(g_atomic_int_get(&ctx->refcount) > 0);
	g_atomic_int_inc(&ctx->refcount);
}

static void fastcgi_fd_cb(struct ev_loop *loop, ev_io *w, int revents);

static fastcgi_connection* fastcgi_connection_new(liVRequest *vr, fastcgi_context *ctx) {
	fastcgi_connection* fcon = g_slice_new0(fastcgi_connection);

	fastcgi_context_acquire(ctx);
	fcon->ctx = ctx;
	fcon->vr = vr;
	fcon->fd = -1;
	ev_init(&fcon->fd_watcher, fastcgi_fd_cb);
	ev_io_set(&fcon->fd_watcher, -1, 0);
	fcon->fd_watcher.data = fcon;
	fcon->fcgi_in = li_chunkqueue_new();
	fcon->fcgi_out = li_chunkqueue_new();
	fcon->stdout = li_chunkqueue_new();
	fcon->buf_in_record = g_byte_array_sized_new(FCGI_HEADER_LEN);
	fcon->requestid = 1;
	fcon->state = FS_WAIT_FOR_REQUEST;
	li_http_response_parser_init(&fcon->parse_response_ctx, &vr->response, fcon->stdout, TRUE, FALSE);
	fcon->response_headers_finished = FALSE;
	return fcon;
}

static void fastcgi_connection_free(fastcgi_connection *fcon) {
	liVRequest *vr;
	if (!fcon) return;

	vr = fcon->vr;
	ev_io_stop(vr->wrk->loop, &fcon->fd_watcher);
	fastcgi_context_release(fcon->ctx);
	if (fcon->fd != -1) close(fcon->fd);
	li_vrequest_backend_finished(vr);

	li_chunkqueue_free(fcon->fcgi_in);
	li_chunkqueue_free(fcon->fcgi_out);
	li_chunkqueue_free(fcon->stdout);
	li_buffer_release(fcon->fcgi_in_buffer);
	g_byte_array_free(fcon->buf_in_record, TRUE);

	li_http_response_parser_clear(&fcon->parse_response_ctx);

	g_slice_free(fastcgi_connection, fcon);
}

/**********************************************************************************/
/* fastcgi stream helper */

static const gchar __padding[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };

static void append_padding(GByteArray *a, guint8 padlen) {
	g_byte_array_append(a, (guint8*) __padding, padlen);
}

static void l_byte_array_append_c(GByteArray *a, char c) {
	g_byte_array_append(a, (guint8*) &c, 1);
}

/* returns padding length */
static guint8 stream_build_fcgi_record(GByteArray *buf, guint8 type, guint16 requestid, guint16 datalen) {
	guint16 w;
	guint8 padlen = (8 - (datalen & 0x7)) % 8; /* padding must be < 8 */

	g_byte_array_set_size(buf, FCGI_HEADER_LEN);
	g_byte_array_set_size(buf, 0);

	l_byte_array_append_c(buf, FCGI_VERSION_1);
	l_byte_array_append_c(buf, type);
	w = htons(requestid);
	g_byte_array_append(buf, (const guint8*) &w, sizeof(w));
	w = htons(datalen);
	g_byte_array_append(buf, (const guint8*) &w, sizeof(w));
	l_byte_array_append_c(buf, padlen);
	l_byte_array_append_c(buf, 0);
	return padlen;
}

/* returns padding length */
static guint8 stream_send_fcgi_record(liChunkQueue *out, guint8 type, guint16 requestid, guint16 datalen) {
	GByteArray *record = g_byte_array_sized_new(FCGI_HEADER_LEN);
	guint8 padlen = stream_build_fcgi_record(record, type, requestid, datalen);
	li_chunkqueue_append_bytearr(out, record);
	return padlen;
}

static void stream_send_data(liChunkQueue *out, guint8 type, guint16 requestid, const gchar *data, size_t datalen) {
	while (datalen > 0) {
		guint16 tosend = (datalen > G_MAXUINT16) ? G_MAXUINT16 : datalen;
		guint8 padlen = stream_send_fcgi_record(out, type, requestid, tosend);
		GByteArray *tmpa = g_byte_array_sized_new(tosend + padlen);
		g_byte_array_append(tmpa, (const guint8*) data, tosend);
		append_padding(tmpa, padlen);
		li_chunkqueue_append_bytearr(out, tmpa);
		data += tosend;
		datalen -= tosend;
	}
}

/* kills the data */
static void stream_send_bytearr(liChunkQueue *out, guint8 type, guint16 requestid, GByteArray *data) {
	if (data->len > G_MAXUINT16) {
		stream_send_data(out, type, requestid, (const gchar*) data->data, data->len);
		g_byte_array_free(data, TRUE);
	} else {
		guint8 padlen = stream_send_fcgi_record(out, type, requestid, data->len);
		append_padding(data, padlen);
		li_chunkqueue_append_bytearr(out, data);
	}
}

static void stream_send_chunks(liChunkQueue *out, guint8 type, guint16 requestid, liChunkQueue *in) {
	while (in->length > 0) {
		guint16 tosend = (in->length > G_MAXUINT16) ? G_MAXUINT16 : in->length;
		guint8 padlen = stream_send_fcgi_record(out, type, requestid, tosend);
		li_chunkqueue_steal_len(out, in, tosend);
		li_chunkqueue_append_mem(out, __padding, padlen);
	}

	if (in->is_closed && !out->is_closed) {
		out->is_closed = TRUE;
		stream_send_fcgi_record(out, type, requestid, 0);
	}
}

static gboolean _append_ba_len(GByteArray *a, size_t len) {
	if (len > G_MAXINT32) return FALSE;
	if (len > 127) {
		guint32 i = htonl(len | (1 << 31));
		g_byte_array_append(a, (const guint8*) &i, sizeof(i));
	} else {
		l_byte_array_append_c(a, (guint8) len);
	}
	return TRUE;
}

static gboolean append_key_value_pair(GByteArray *a, const gchar *key, size_t keylen, const gchar *val, size_t valuelen) {
	if (!_append_ba_len(a, keylen) || !_append_ba_len(a, valuelen)) return FALSE;
	g_byte_array_append(a, (const guint8*) key, keylen);
	g_byte_array_append(a, (const guint8*) val, valuelen);
	return TRUE;
}

/**********************************************************************************/

static void fastcgi_send_begin(fastcgi_connection *fcon) {
	GByteArray *buf = g_byte_array_sized_new(16);
	guint16 w;

	stream_build_fcgi_record(buf, FCGI_BEGIN_REQUEST, fcon->requestid, 8);
	w = htons(FCGI_RESPONDER);
	g_byte_array_append(buf, (const guint8*) &w, sizeof(w));
	l_byte_array_append_c(buf, 0); /* TODO: FCGI_KEEP_CONN */
	append_padding(buf, 5);
	li_chunkqueue_append_bytearr(fcon->fcgi_out, buf);
}

static void fastcgi_env_add(GByteArray *buf, liEnvironmentDup *envdup, const gchar *key, size_t keylen, const gchar *val, size_t valuelen) {
	GString *sval;

	if (NULL != (sval = li_environment_dup_pop(envdup, key, keylen))) {
		append_key_value_pair(buf, key, keylen, GSTR_LEN(sval));
	} else {
		append_key_value_pair(buf, key, keylen, val, valuelen);
	}
}

static void fastcgi_env_create(liVRequest *vr, liEnvironmentDup *envdup, GByteArray* buf) {
	liConnection *con = vr->con;
	GString *tmp = con->wrk->tmp_str;

	fastcgi_env_add(buf, envdup, CONST_STR_LEN("SERVER_SOFTWARE"), GSTR_LEN(CORE_OPTIONPTR(LI_CORE_OPTION_SERVER_TAG).string));
	fastcgi_env_add(buf, envdup, CONST_STR_LEN("SERVER_NAME"), GSTR_LEN(vr->request.uri.host));
	fastcgi_env_add(buf, envdup, CONST_STR_LEN("GATEWAY_INTERFACE"), CONST_STR_LEN("CGI/1.1"));
	{
		guint port = 0;
		switch (con->local_addr.addr->plain.sa_family) {
		case AF_INET: port = con->local_addr.addr->ipv4.sin_port; break;
#ifdef HAVE_IPV6
		case AF_INET6: port = con->local_addr.addr->ipv6.sin6_port; break;
#endif
		}
		if (port) {
			g_string_printf(tmp, "%u", htons(port));
			fastcgi_env_add(buf, envdup, CONST_STR_LEN("SERVER_PORT"), GSTR_LEN(tmp));
		}
	}
	fastcgi_env_add(buf, envdup, CONST_STR_LEN("SERVER_ADDR"), GSTR_LEN(con->local_addr_str));

	{
		guint port = 0;
		switch (con->remote_addr.addr->plain.sa_family) {
		case AF_INET: port = con->remote_addr.addr->ipv4.sin_port; break;
#ifdef HAVE_IPV6
		case AF_INET6: port = con->remote_addr.addr->ipv6.sin6_port; break;
#endif
		}
		if (port) {
			g_string_printf(tmp, "%u", htons(port));
			fastcgi_env_add(buf, envdup, CONST_STR_LEN("REMOTE_PORT"), GSTR_LEN(tmp));
		}
	}
	fastcgi_env_add(buf, envdup, CONST_STR_LEN("REMOTE_ADDR"), GSTR_LEN(con->remote_addr_str));

	if (vr->request.content_length > 0) {
		g_string_printf(tmp, "%" L_GOFFSET_MODIFIER "i", vr->request.content_length);
		fastcgi_env_add(buf, envdup, CONST_STR_LEN("CONTENT_LENGTH"), GSTR_LEN(tmp));
	}

	fastcgi_env_add(buf, envdup, CONST_STR_LEN("SCRIPT_NAME"), GSTR_LEN(vr->request.uri.path));

	fastcgi_env_add(buf, envdup, CONST_STR_LEN("PATH_INFO"), GSTR_LEN(vr->physical.pathinfo));
	if (vr->physical.pathinfo->len) {
		g_string_truncate(tmp, 0);
		g_string_append_len(tmp, GSTR_LEN(vr->physical.doc_root)); /* TODO: perhaps an option for alternative doc-root? */
		g_string_append_len(tmp, GSTR_LEN(vr->physical.pathinfo));
		fastcgi_env_add(buf, envdup, CONST_STR_LEN("PATH_TRANSLATED"), GSTR_LEN(tmp));
	}

	fastcgi_env_add(buf, envdup, CONST_STR_LEN("SCRIPT_FILENAME"), GSTR_LEN(vr->physical.path));
	fastcgi_env_add(buf, envdup, CONST_STR_LEN("DOCUMENT_ROOT"), GSTR_LEN(vr->physical.doc_root));

	fastcgi_env_add(buf, envdup, CONST_STR_LEN("REQUEST_URI"), GSTR_LEN(vr->request.uri.raw_orig_path));
	if (!g_string_equal(vr->request.uri.raw_orig_path, vr->request.uri.raw_path)) {
		fastcgi_env_add(buf, envdup, CONST_STR_LEN("REDIRECT_URI"), GSTR_LEN(vr->request.uri.raw_path));
	}
	fastcgi_env_add(buf, envdup, CONST_STR_LEN("QUERY_STRING"), GSTR_LEN(vr->request.uri.query));

	fastcgi_env_add(buf, envdup, CONST_STR_LEN("REQUEST_METHOD"), GSTR_LEN(vr->request.http_method_str));
	fastcgi_env_add(buf, envdup, CONST_STR_LEN("REDIRECT_STATUS"), CONST_STR_LEN("200")); /* if php is compiled with --force-redirect */
	switch (vr->request.http_version) {
	case LI_HTTP_VERSION_1_1:
		fastcgi_env_add(buf, envdup, CONST_STR_LEN("SERVER_PROTOCOL"), CONST_STR_LEN("HTTP/1.1"));
		break;
	case LI_HTTP_VERSION_1_0:
	default:
		fastcgi_env_add(buf, envdup, CONST_STR_LEN("SERVER_PROTOCOL"), CONST_STR_LEN("HTTP/1.0"));
		break;
	}

	if (con->is_ssl) {
		fastcgi_env_add(buf, envdup, CONST_STR_LEN("HTTPS"), CONST_STR_LEN("on"));
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

static void fastcgi_send_env(liVRequest *vr, fastcgi_connection *fcon) {
	GByteArray *buf = g_byte_array_sized_new(0);
	liEnvironmentDup *envdup;

	envdup = li_environment_make_dup(&vr->env);
	fastcgi_env_create(vr, envdup, buf);

	{
		GList *i;
		GString *tmp = vr->wrk->tmp_str;

		for (i = vr->request.headers->entries.head; NULL != i; i = i->next) {
			liHttpHeader *h = (liHttpHeader*) i->data;
			const GString hkey = li_const_gstring(h->data->str, h->keylen);
			g_string_truncate(tmp, 0);
			if (!li_strncase_equal(&hkey, CONST_STR_LEN("CONTENT-TYPE"))) {
				g_string_append_len(tmp, CONST_STR_LEN("HTTP_"));
			}
			g_string_append_len(tmp, h->data->str, h->keylen);
			fix_header_name(tmp);
	
			fastcgi_env_add(buf, envdup, GSTR_LEN(tmp), h->data->str + h->keylen+2, h->data->len - (h->keylen+2));
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

	stream_send_bytearr(fcon->fcgi_out, FCGI_PARAMS, fcon->requestid, buf);
	stream_send_fcgi_record(fcon->fcgi_out, FCGI_PARAMS, fcon->requestid, 0);
}

static void fastcgi_forward_request(liVRequest *vr, fastcgi_connection *fcon) {
	stream_send_chunks(fcon->fcgi_out, FCGI_STDIN, fcon->requestid, vr->in);
	if (fcon->fcgi_out->length > 0)
		li_ev_io_add_events(vr->wrk->loop, &fcon->fd_watcher, EV_WRITE);
}

static gboolean fastcgi_get_packet(fastcgi_connection *fcon) {
	const unsigned char *data;

	/* already got packet */
	if (fcon->fcgi_in_record.valid) {
		if (0 == fcon->fcgi_in_record.remainingContent) {
			/* wait for padding data ? */
			gint len = fcon->fcgi_in->length;
			if (len > fcon->fcgi_in_record.remainingPadding) len = fcon->fcgi_in_record.remainingPadding;
			li_chunkqueue_skip(fcon->fcgi_in, len);
			fcon->fcgi_in_record.remainingPadding -= len;
			if (0 != fcon->fcgi_in_record.remainingPadding) return FALSE; /* wait for data */
			fcon->fcgi_in_record.valid = FALSE; /* read next packet */
		} else {
			return (fcon->fcgi_in->length > 0); /* wait for/handle more content */
		}
	}

	if (!li_chunkqueue_extract_to_bytearr(fcon->vr, fcon->fcgi_in, FCGI_HEADER_LEN, fcon->buf_in_record)) return FALSE; /* need more data */

	data = (const unsigned char*) fcon->buf_in_record->data;
	fcon->fcgi_in_record.version = data[0];
	fcon->fcgi_in_record.type = data[1];
	fcon->fcgi_in_record.requestID = (data[2] << 8) | (data[3]);
	fcon->fcgi_in_record.contentLength = (data[4] << 8) | (data[5]);
	fcon->fcgi_in_record.paddingLength = data[6];
	fcon->fcgi_in_record.remainingContent = fcon->fcgi_in_record.contentLength;
	fcon->fcgi_in_record.remainingPadding = fcon->fcgi_in_record.paddingLength;
	fcon->fcgi_in_record.valid = TRUE;
	fcon->fcgi_in_record.first = TRUE;

	li_chunkqueue_skip(fcon->fcgi_in, FCGI_HEADER_LEN);

	return TRUE;
}

/* get available data and mark it as read (subtract it from contentLength) */
static int fastcgi_available(fastcgi_connection *fcon) {
	gint len = fcon->fcgi_in->length;
	if (len > fcon->fcgi_in_record.remainingContent) len = fcon->fcgi_in_record.remainingContent;
	fcon->fcgi_in_record.remainingContent -= len;
	return len;
}

static gboolean fastcgi_parse_response(fastcgi_connection *fcon) {
	liVRequest *vr = fcon->vr;
	liPlugin *p = fcon->ctx->plugin;
	gint len;
	while (fastcgi_get_packet(fcon)) {
		if (fcon->fcgi_in_record.version != FCGI_VERSION_1) {
			VR_ERROR(vr, "(%s) Unknown fastcgi protocol version %i", fcon->ctx->socket_str->str, (gint) fcon->fcgi_in_record.version);
			close(fcon->fd);
			fcon->fd = -1;
			li_vrequest_error(vr);
			return FALSE;
		}
		switch (fcon->fcgi_in_record.type) {
		case FCGI_END_REQUEST:
			li_chunkqueue_skip(fcon->fcgi_in, fastcgi_available(fcon));
			fcon->stdout->is_closed = TRUE;
			break;
		case FCGI_STDOUT:
			if (0 == fcon->fcgi_in_record.contentLength) {
				fcon->stdout->is_closed = TRUE;
			} else {
				li_chunkqueue_steal_len(fcon->stdout, fcon->fcgi_in, fastcgi_available(fcon));
			}
			break;
		case FCGI_STDERR:
			len = fastcgi_available(fcon);
			li_chunkqueue_extract_to(vr, fcon->fcgi_in, len, vr->wrk->tmp_str);
			if (OPTION(FASTCGI_OPTION_LOG_PLAIN_ERRORS).boolean) {
				li_log_split_lines(vr->wrk->srv, vr, LI_LOG_LEVEL_BACKEND, 0, vr->wrk->tmp_str->str, "");
			} else {
				VR_BACKEND_LINES(vr, vr->wrk->tmp_str->str, "(fcgi-stderr %s) ", fcon->ctx->socket_str->str);
			}
			li_chunkqueue_skip(fcon->fcgi_in, len);
			break;
		default:
			if (fcon->fcgi_in_record.first) VR_WARNING(vr, "(%s) Unhandled fastcgi record type %i", fcon->ctx->socket_str->str, (gint) fcon->fcgi_in_record.type);
			li_chunkqueue_skip(fcon->fcgi_in, fastcgi_available(fcon));
			break;
		}
		fcon->fcgi_in_record.first = FALSE;
	}
	return TRUE;
}

/**********************************************************************************/

static liHandlerResult fastcgi_statemachine(liVRequest *vr, fastcgi_connection *fcon);

static void fastcgi_fd_cb(struct ev_loop *loop, ev_io *w, int revents) {
	fastcgi_connection *fcon = (fastcgi_connection*) w->data;

	if (fcon->state == FS_CONNECTING) {
		if (LI_HANDLER_GO_ON != fastcgi_statemachine(fcon->vr, fcon)) {
			li_vrequest_error(fcon->vr);
		}
		return;
	}

	if (revents & EV_READ) {
		if (fcon->fcgi_in->is_closed) {
			li_ev_io_rem_events(loop, w, EV_READ);
		} else {
			switch (li_network_read(fcon->vr, w->fd, fcon->fcgi_in, &fcon->fcgi_in_buffer)) {
			case LI_NETWORK_STATUS_SUCCESS:
				break;
			case LI_NETWORK_STATUS_FATAL_ERROR:
				VR_ERROR(fcon->vr, "(%s) network read fatal error", fcon->ctx->socket_str->str);
				li_vrequest_error(fcon->vr);
				return;
			case LI_NETWORK_STATUS_CONNECTION_CLOSE:
				fcon->fcgi_in->is_closed = TRUE;
				ev_io_stop(loop, w);
				close(fcon->fd);
				fcon->fd = -1;
				li_vrequest_backend_finished(fcon->vr);
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

	if (fcon->fd != -1 && (revents & EV_WRITE)) {
		if (fcon->fcgi_out->length > 0) {
			switch (li_network_write(fcon->vr, w->fd, fcon->fcgi_out, 256*1024)) {
			case LI_NETWORK_STATUS_SUCCESS:
				break;
			case LI_NETWORK_STATUS_FATAL_ERROR:
				VR_ERROR(fcon->vr, "(%s) network write fatal error", fcon->ctx->socket_str->str);
				li_vrequest_error(fcon->vr);
				return;
			case LI_NETWORK_STATUS_CONNECTION_CLOSE:
				fcon->fcgi_in->is_closed = TRUE;
				ev_io_stop(loop, w);
				close(fcon->fd);
				fcon->fd = -1;
				li_vrequest_backend_finished(fcon->vr);
				break;
			case LI_NETWORK_STATUS_WAIT_FOR_EVENT:
				break;
			case LI_NETWORK_STATUS_WAIT_FOR_AIO_EVENT:
				li_ev_io_rem_events(loop, w, EV_WRITE);
				/* TODO: aio */
				break;
			}
		}
		if (fcon->fcgi_out->length == 0) {
			li_ev_io_rem_events(loop, w, EV_WRITE);
		}
	}

	if (!fastcgi_parse_response(fcon)) return;

	if (!fcon->response_headers_finished) {
		switch (li_http_response_parse(fcon->vr, &fcon->parse_response_ctx)) {
		case LI_HANDLER_GO_ON:
			fcon->response_headers_finished = TRUE;
			li_vrequest_handle_response_headers(fcon->vr);
			break;
		case LI_HANDLER_ERROR:
			VR_ERROR(fcon->vr, "Parsing response header failed for: %s", fcon->ctx->socket_str->str);
			li_vrequest_error(fcon->vr);
			break;
		default:
			break;
		}
	}

	if (fcon->response_headers_finished) {
		li_chunkqueue_steal_all(fcon->vr->out, fcon->stdout);
		fcon->vr->out->is_closed = fcon->stdout->is_closed;
		li_vrequest_handle_response_body(fcon->vr);
	}

	if (fcon->fcgi_in->is_closed && !fcon->vr->out->is_closed) {
		VR_ERROR(fcon->vr, "(%s) unexpected end-of-file (perhaps the fastcgi process died)", fcon->ctx->socket_str->str);
		li_vrequest_error(fcon->vr);
	}
}

/**********************************************************************************/
/* state machine */

static void fastcgi_close(liVRequest *vr, liPlugin *p);

static liHandlerResult fastcgi_statemachine(liVRequest *vr, fastcgi_connection *fcon) {
	liPlugin *p = fcon->ctx->plugin;

	switch (fcon->state) {
	case FS_WAIT_FOR_REQUEST:
		/* wait until we have either all data or the cqlimit is full */
		if (-1 == vr->request.content_length || vr->request.content_length != vr->in->length) {
			if (0 != li_chunkqueue_limit_available(vr->in))
				return LI_HANDLER_GO_ON;
		}
		fcon->state = FS_CONNECT;

		/* fall through */
	case FS_CONNECT:
		do {
			fcon->fd = socket(fcon->ctx->socket.addr->plain.sa_family, SOCK_STREAM, 0);
		} while (-1 == fcon->fd && errno == EINTR);
		if (-1 == fcon->fd) {
			if (errno == EMFILE) {
				li_server_out_of_fds(vr->wrk->srv);
			} else if (errno != g_atomic_int_get(&fcon->ctx->last_errno)) {
				g_atomic_int_set(&fcon->ctx->last_errno, errno);
				VR_ERROR(vr, "Couldn't open socket: %s", g_strerror(errno));
			}
			return LI_HANDLER_ERROR;
		}
		li_fd_init(fcon->fd);
		ev_io_set(&fcon->fd_watcher, fcon->fd, EV_READ | EV_WRITE);
		ev_io_start(vr->wrk->loop, &fcon->fd_watcher);

		/* fall through */
	case FS_CONNECTING:
		if (-1 == connect(fcon->fd, &fcon->ctx->socket.addr->plain, fcon->ctx->socket.len)) {
			switch (errno) {
			case EINPROGRESS:
			case EALREADY:
			case EINTR:
				fcon->state = FS_CONNECTING;
				return LI_HANDLER_GO_ON;
			case EAGAIN: /* backend overloaded */
				fastcgi_close(vr, p);
				li_vrequest_backend_overloaded(vr);
				return LI_HANDLER_GO_ON;
			default:
				if (errno != g_atomic_int_get(&fcon->ctx->last_errno)) {
					g_atomic_int_set(&fcon->ctx->last_errno, errno);
					VR_ERROR(vr, "Couldn't connect to '%s': %s",
						li_sockaddr_to_string(fcon->ctx->socket, vr->wrk->tmp_str, TRUE)->str,
						g_strerror(errno));
				}
				fastcgi_close(vr, p);
				li_vrequest_backend_dead(vr);
				return LI_HANDLER_GO_ON;
			}
		}

		g_atomic_int_set(&fcon->ctx->last_errno, 0);

		fcon->state = FS_CONNECTED;

		/* prepare stream */
		fastcgi_send_begin(fcon);
		fastcgi_send_env(vr, fcon);

		/* fall through */
	case FS_CONNECTED:
		fastcgi_forward_request(vr, fcon);
		break;

	case FS_DONE:
		break;
	}

	return LI_HANDLER_GO_ON;
}


/**********************************************************************************/

static liHandlerResult fastcgi_handle(liVRequest *vr, gpointer param, gpointer *context) {
	fastcgi_context *ctx = (fastcgi_context*) param;
	fastcgi_connection *fcon;
	UNUSED(context);
	if (!li_vrequest_handle_indirect(vr, ctx->plugin)) return LI_HANDLER_GO_ON;

	fcon = fastcgi_connection_new(vr, ctx);
	if (!fcon) {
		return LI_HANDLER_ERROR;
	}
	g_ptr_array_index(vr->plugin_ctx, ctx->plugin->id) = fcon;

	li_chunkqueue_set_limit(fcon->fcgi_in, vr->out->limit);
	li_chunkqueue_set_limit(fcon->stdout, vr->out->limit);
	li_chunkqueue_set_limit(fcon->fcgi_out, vr->in->limit);
	if (vr->out->limit) vr->out->limit->io_watcher = &fcon->fd_watcher;

	return fastcgi_statemachine(vr, fcon);
}


static liHandlerResult fastcgi_handle_request_body(liVRequest *vr, liPlugin *p) {
	fastcgi_connection *fcon = (fastcgi_connection*) g_ptr_array_index(vr->plugin_ctx, p->id);
	if (!fcon) return LI_HANDLER_ERROR;

	return fastcgi_statemachine(vr, fcon);
}

static void fastcgi_close(liVRequest *vr, liPlugin *p) {
	fastcgi_connection *fcon = (fastcgi_connection*) g_ptr_array_index(vr->plugin_ctx, p->id);
	g_ptr_array_index(vr->plugin_ctx, p->id) = NULL;
	if (fcon) {
		if (vr->out->limit) vr->out->limit->io_watcher = NULL;
		fastcgi_connection_free(fcon);
	}
}


static void fastcgi_free(liServer *srv, gpointer param) {
	fastcgi_context *ctx = (fastcgi_context*) param;
	UNUSED(srv);

	fastcgi_context_release(ctx);
}

static liAction* fastcgi_create(liServer *srv, liWorker *wrk, liPlugin* p, liValue *val, gpointer userdata) {
	fastcgi_context *ctx;

	UNUSED(wrk); UNUSED(userdata);

	if (val->type != LI_VALUE_STRING) {
		ERROR(srv, "%s", "fastcgi expects a string as parameter");
		return FALSE;
	}

	ctx = fastcgi_context_new(srv, p, val->data.string);
	if (!ctx) return NULL;

	return li_action_new_function(fastcgi_handle, NULL, fastcgi_free, ctx);
}

static const liPluginOption options[] = {
	{ "fastcgi.log_plain_errors", LI_VALUE_BOOLEAN, FALSE, NULL },

	{ NULL, 0, 0, NULL }
};

static const liPluginAction actions[] = {
	{ "fastcgi", fastcgi_create, NULL },
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

	p->handle_request_body = fastcgi_handle_request_body;
	p->handle_vrclose = fastcgi_close;
}


gboolean mod_fastcgi_init(liModules *mods, liModule *mod) {
	MODULE_VERSION_CHECK(mods);

	mod->config = li_plugin_register(mods->main, "mod_fastcgi", plugin_init, NULL);

	return mod->config != NULL;
}

gboolean mod_fastcgi_free(liModules *mods, liModule *mod) {
	if (mod->config)
		li_plugin_free(mods->main, mod->config);

	return TRUE;
}

#include "fastcgi_stream.h"
#include <lighttpd/plugin_core.h>
#include <lighttpd/stream_http_response.h>


/**********************************************************************************/
/* fastcgi types */

#define FCGI_VERSION_1           1
#define FCGI_HEADER_LEN  8

enum FCGI_Type {
	FCGI_BEGIN_REQUEST     = 1, /* web server -> backend */
	FCGI_ABORT_REQUEST     = 2, /* web server -> backend */
	FCGI_END_REQUEST       = 3, /* backend -> web server (status) */
	FCGI_PARAMS            = 4, /* web server -> backend (stream name-value pairs) */
	FCGI_STDIN             = 5, /* web server -> backend (stream request body) */
	FCGI_STDOUT            = 6, /* backend -> web server (stream response body) */
	FCGI_STDERR            = 7, /* backend -> web server (stream error messages) */
	FCGI_DATA              = 8, /* web server -> backend (stream additional data) */
	FCGI_GET_VALUES        = 9, /* web server -> backend (request names-value pairs with empty values) */
	FCGI_GET_VALUES_RESULT = 10,/* backend -> web server (response name-value pairs) */
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

typedef struct liFastCGIBackendContext liFastCGIBackendContext;
typedef struct liFastCGIBackendConnection_p liFastCGIBackendConnection_p;
typedef struct liFastCGIBackendPool_p liFastCGIBackendPool_p;

struct liFastCGIBackendContext {
	gint refcount;
	liFastCGIBackendPool_p *pool;
	liBackendConnection *subcon;
	gboolean is_active; /* if is_active == FALSE iostream->io_watcher must not have a ref on the loop */

	liWorker *wrk;
	liIOStream *iostream;

	liStream fcgi_out, fcgi_in;

	/* for now: no multiplexing, at most one connection */
	liFastCGIBackendConnection_p *currentcon;
	gboolean stdin_closed, stdout_closed, stderr_closed, request_done;

	/* current record */
	guint8 version;
	guint8 type;
	guint16 requestID;
	guint16 contentLength;
	guint8 paddingLength;
	gint remainingContent, remainingPadding;
};

struct liFastCGIBackendConnection_p {
	liFastCGIBackendConnection public;
	liFastCGIBackendContext *ctx;

	liVRequest *vr;
};

struct liFastCGIBackendPool_p {
	liFastCGIBackendPool public;
	const liFastCGIBackendCallbacks *callbacks;

	liBackendConfig config;
};

/* debug */
#if 0
#define STRINGIFY(x) #x
#define _STRINGIFY(x) STRINGIFY(x)
#define fcgi_debug(...) fprintf(stderr, "fastcgi-stream.c:" _STRINGIFY(__LINE__) ": " __VA_ARGS__)
#define FCGI_DEBUG

static const gchar* fcgi_type_string(enum FCGI_Type type) {
	switch (type) {
	case FCGI_BEGIN_REQUEST:
		return "begin_request";
	case FCGI_ABORT_REQUEST:
		return "abort_request";
	case FCGI_END_REQUEST:
		return "end_request";
	case FCGI_PARAMS:
		return "params";
	case FCGI_STDIN:
		return "stdin";
	case FCGI_STDOUT:
		return "stdout";
	case FCGI_STDERR:
		return "stderr";
	case FCGI_DATA:
		return "data";
	case FCGI_GET_VALUES:
		return "get_values";
	case FCGI_GET_VALUES_RESULT:
		return "get_values_result";
	default:
		return "unknown_type";
	}
}
#else
#define fcgi_debug(...) do { } while (0)
#endif

static void fastcgi_stream_out(liStream *stream, liStreamEvent event);
static void fastcgi_stream_in(liStream *stream, liStreamEvent event);

static void backend_detach_thread(liBackendPool *bpool, liWorker *wrk, liBackendConnection *bcon) {
	liFastCGIBackendContext *ctx = bcon->data;
	UNUSED(bpool);

	assert(wrk == ctx->wrk);
	ctx->wrk = NULL;

	li_stream_disconnect(&ctx->fcgi_out);
	li_stream_disconnect_dest(&ctx->fcgi_in);

	assert(2 == ctx->fcgi_in.refcount);
	assert(2 == ctx->fcgi_out.refcount);

	li_iostream_detach(ctx->iostream);
	li_stream_detach(&ctx->fcgi_out);
	li_stream_detach(&ctx->fcgi_in);
}

static void backend_attach_thread(liBackendPool *bpool, liWorker *wrk, liBackendConnection *bcon) {
	liFastCGIBackendContext *ctx = bcon->data;
	UNUSED(bpool);

	ctx->wrk = wrk;
	li_iostream_attach(ctx->iostream, wrk);
	li_stream_attach(&ctx->fcgi_out, &wrk->loop);
	li_stream_attach(&ctx->fcgi_in, &wrk->loop);
}

static void backend_new(liBackendPool *bpool, liWorker *wrk, liBackendConnection *bcon) {
	liFastCGIBackendPool_p *pool = LI_CONTAINER_OF(bpool->config, liFastCGIBackendPool_p, config);
	liFastCGIBackendContext *ctx = g_slice_new0(liFastCGIBackendContext);

	fcgi_debug("%s\n", "backend_new");

	ctx->refcount = 3; /* backend_close, fcgi_out, fcgi_in */
	ctx->pool = pool;
	ctx->wrk = wrk;
	ctx->iostream = li_iostream_new(wrk, li_event_io_fd(&bcon->watcher), li_stream_simple_socket_io_cb, NULL);
	li_event_set_keep_loop_alive(&ctx->iostream->io_watcher, FALSE);

	li_stream_init(&ctx->fcgi_out, &wrk->loop, fastcgi_stream_out);
	li_stream_init(&ctx->fcgi_in, &wrk->loop, fastcgi_stream_in);

	li_stream_connect(&ctx->iostream->stream_in, &ctx->fcgi_in);
	li_stream_connect(&ctx->fcgi_out, &ctx->iostream->stream_out);

	ctx->subcon = bcon;
	bcon->data = ctx;
}

static void backend_ctx_unref(liFastCGIBackendContext *ctx) {
	assert(g_atomic_int_get(&ctx->refcount) > 0);
	if (g_atomic_int_dec_and_test(&ctx->refcount)) {
		g_slice_free(liFastCGIBackendContext, ctx);
	}
}

static void backend_close(liBackendPool *bpool, liWorker *wrk, liBackendConnection *bcon) {
	liFastCGIBackendContext *ctx = bcon->data;
	UNUSED(bpool);

	assert(NULL != ctx->pool);
	assert(wrk == ctx->wrk);

	ctx->pool = NULL;

	assert(NULL == ctx->currentcon);

	fcgi_debug("%s\n", "backend_close");

	if (NULL != ctx->iostream) {
		li_stream_simple_socket_close(ctx->iostream, FALSE);
		li_iostream_reset(ctx->iostream);
		ctx->iostream = NULL;
	}
	li_stream_reset(&ctx->fcgi_in);
	li_stream_reset(&ctx->fcgi_out);

	li_stream_release(&ctx->fcgi_in);
	li_stream_release(&ctx->fcgi_out);

	backend_ctx_unref(ctx);

	li_event_io_set_fd(&bcon->watcher, -1);
}

static void backend_free(liBackendPool *bpool) {
	liFastCGIBackendPool_p *pool = LI_CONTAINER_OF(bpool->config, liFastCGIBackendPool_p, config);

	li_sockaddr_clear(&pool->config.sock_addr);

	g_slice_free(liFastCGIBackendPool_p, pool);
}

static liBackendCallbacks backend_cbs = {
	backend_detach_thread,
	backend_attach_thread,
	backend_new,
	backend_close,
	backend_free
};

static void fastcgi_check_put(liFastCGIBackendContext *ctx) {
	/* wait for li_fastcgi_backend_put() */
	if (NULL != ctx->currentcon) return;
	/* already inactive */
	if (!ctx->is_active) return;
	/* wait for vrequest streams to disconnect */
	if (NULL != ctx->fcgi_in.dest || NULL != ctx->fcgi_out.source) return;

	ctx->is_active = FALSE;

	li_stream_set_cqlimit(NULL, &ctx->fcgi_in, NULL);
	li_stream_set_cqlimit(&ctx->fcgi_out, NULL, NULL);

	if (NULL != ctx->iostream) {
		li_event_io_set_fd(&ctx->subcon->watcher, li_event_io_fd(&ctx->iostream->io_watcher));
		li_event_set_keep_loop_alive(&ctx->iostream->io_watcher, FALSE);
		assert(NULL == ctx->iostream->stream_in.out->limit);
		assert(NULL == ctx->iostream->stream_out.out->limit);
	} else {
		li_event_io_set_fd(&ctx->subcon->watcher, -1);
	}

	assert(NULL == ctx->fcgi_in.out->limit);
	assert(NULL == ctx->fcgi_out.out->limit);

	fcgi_debug("%s\n", "li_backend_put");
	li_backend_put(ctx->wrk, ctx->pool->public.subpool, ctx->subcon, FALSE);
}

/* destroys ctx */
static void fastcgi_reset(liFastCGIBackendContext *ctx) {
	if (NULL == ctx->pool) return;
	fcgi_debug("%s\n", "fastcgi_reset");

	if (!ctx->is_active) {
		li_backend_connection_closed(ctx->pool->public.subpool, ctx->subcon);
	} else {
		const liFastCGIBackendCallbacks *callbacks = ctx->pool->callbacks;
		liFastCGIBackendConnection_p *currentcon = ctx->currentcon;
		liIOStream *iostream = ctx->iostream;

		if (NULL == iostream) return;

		ctx->request_done = TRUE;
		ctx->iostream = NULL;
		li_stream_simple_socket_close(iostream, TRUE);
		li_iostream_reset(iostream);

		li_stream_disconnect(&ctx->fcgi_out);
		li_stream_disconnect_dest(&ctx->fcgi_in);

		if (NULL != currentcon) {
			callbacks->reset_cb(currentcon->vr, &ctx->pool->public, &currentcon->public);
		}
	}
}

/**********************************************************************************/
/* fastcgi stream send helper */

static const gchar __padding[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };

static void append_padding(GByteArray *a, guint8 padlen) {
	g_byte_array_append(a, (guint8*) __padding, padlen);
}

static void l_byte_array_append_c(GByteArray *a, char c) {
	g_byte_array_append(a, (guint8*) &c, 1);
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
}

static void stream_send_begin(liChunkQueue *out, guint16 requestid) {
	GByteArray *buf = g_byte_array_sized_new(16);
	guint16 w;

	assert(1 == requestid);

	stream_build_fcgi_record(buf, FCGI_BEGIN_REQUEST, requestid, 8);
	w = htons(FCGI_RESPONDER);
	g_byte_array_append(buf, (const guint8*) &w, sizeof(w));
	l_byte_array_append_c(buf, FCGI_KEEP_CONN);
	append_padding(buf, 5);
	li_chunkqueue_append_bytearr(out, buf);
}

/* end fastcgi stream send helpers */
/**********************************************************************************/

/**********************************************************************************/
/* fastcgi environment build helpers */
static void fastcgi_env_add(GByteArray *buf, liEnvironmentDup *envdup, const gchar *key, size_t keylen, const gchar *val, size_t valuelen) {
	GString *sval;

	if (NULL != (sval = li_environment_dup_pop(envdup, key, keylen))) {
		append_key_value_pair(buf, key, keylen, GSTR_LEN(sval));
	} else {
		append_key_value_pair(buf, key, keylen, val, valuelen);
	}
}

static void fastcgi_env_create(liVRequest *vr, liEnvironmentDup *envdup, GByteArray* buf) {
	liConInfo *coninfo = vr->coninfo;
	GString *tmp = vr->wrk->tmp_str;

	fastcgi_env_add(buf, envdup, CONST_STR_LEN("SERVER_SOFTWARE"), GSTR_LEN(CORE_OPTIONPTR(LI_CORE_OPTION_SERVER_TAG).string));
	fastcgi_env_add(buf, envdup, CONST_STR_LEN("SERVER_NAME"), GSTR_LEN(vr->request.uri.host));
	fastcgi_env_add(buf, envdup, CONST_STR_LEN("GATEWAY_INTERFACE"), CONST_STR_LEN("CGI/1.1"));
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
			fastcgi_env_add(buf, envdup, CONST_STR_LEN("SERVER_PORT"), GSTR_LEN(tmp));
		}
	}
	fastcgi_env_add(buf, envdup, CONST_STR_LEN("SERVER_ADDR"), GSTR_LEN(coninfo->local_addr_str));

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
			fastcgi_env_add(buf, envdup, CONST_STR_LEN("REMOTE_PORT"), GSTR_LEN(tmp));
		}
	}
	fastcgi_env_add(buf, envdup, CONST_STR_LEN("REMOTE_ADDR"), GSTR_LEN(coninfo->remote_addr_str));

	if (vr->request.content_length > 0) {
		g_string_printf(tmp, "%" LI_GOFFSET_MODIFIER "i", vr->request.content_length);
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

	if (coninfo->is_ssl) {
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

static void fastcgi_send_env(liVRequest *vr, liChunkQueue *out, int requestid) {
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

	if (buf->len > 0) stream_send_bytearr(out, FCGI_PARAMS, requestid, buf);
	stream_send_fcgi_record(out, FCGI_PARAMS, requestid, 0);
}

/* end fastcgi environment build helpers */
/**********************************************************************************/


/* request body -> fastcgi */
static void fastcgi_stream_out(liStream *stream, liStreamEvent event) {
	liFastCGIBackendContext *ctx = LI_CONTAINER_OF(stream, liFastCGIBackendContext, fcgi_out);
	fcgi_debug("fastcgi_stream_out event: %s\n", li_stream_event_string(event));
	switch (event) {
	case LI_STREAM_NEW_DATA:
		if (NULL == stream->source) return;
		if (NULL == stream->dest || ctx->stdin_closed) {
			li_chunkqueue_skip_all(stream->source->out);
			return;
		}
		stream_send_chunks(stream->out, FCGI_STDIN, 1, stream->source->out);
		if (stream->source->out->is_closed && !ctx->stdin_closed) {
			fcgi_debug("fcgi_out: closing stdin\n");
			ctx->stdin_closed = TRUE;
			stream_send_fcgi_record(stream->out, FCGI_STDIN, 1, 0);
			li_stream_disconnect(stream);
		}
		li_stream_notify(stream);
		break;
	case LI_STREAM_CONNECTED_SOURCE:
		assert(!ctx->stdin_closed);
		break;
	case LI_STREAM_DISCONNECTED_SOURCE:
		if (!ctx->stdin_closed) {
			fastcgi_reset(ctx);
		} else {
			fastcgi_check_put(ctx);
		}
		break;
	case LI_STREAM_DISCONNECTED_DEST:
		if (stream->out->length > 0) {
			fcgi_debug("fcgi_out: lost iostream");
			li_chunkqueue_skip_all(stream->out);
		}
		break;
	case LI_STREAM_DESTROY:
		backend_ctx_unref(ctx);
	default:
		break;
	}
}

static void fastcgi_decode(liFastCGIBackendContext *ctx) {
	liChunkQueue *in = ctx->iostream->stream_in.out;
	liWorker *wrk = li_worker_from_iostream(ctx->iostream);

	while (NULL != ctx->iostream && 0 < in->length) {
		gboolean newdata = FALSE;

		if (0 == ctx->remainingContent && 0 == ctx->remainingPadding) {
			unsigned char header[FCGI_HEADER_LEN];
			if (in->length < FCGI_HEADER_LEN) break;

			/* reading memory buffers can't fail */
			if (!li_chunkqueue_extract_to_memory(in, FCGI_HEADER_LEN, header, NULL)) abort();
			li_chunkqueue_skip(in, FCGI_HEADER_LEN);

			ctx->version = header[0];
			ctx->type = header[1];
			ctx->requestID = (header[2] << 8) + header[3];
			ctx->contentLength = (header[4] << 8) + header[5];
			ctx->paddingLength = header[6];

			ctx->remainingContent = ctx->contentLength;
			ctx->remainingPadding = ctx->paddingLength;

			if (FCGI_VERSION_1 != ctx->version) {
				ERROR(wrk->srv, "(%s) Unknown fastcgi protocol version %i",
					li_sockaddr_to_string(ctx->pool->config.sock_addr, wrk->tmp_str, TRUE)->str,
					(gint) ctx->version);
				fastcgi_reset(ctx);
				return;
			}
			newdata = TRUE;
			fcgi_debug("fastcgi packet type %s (%i), payload %i\n", fcgi_type_string(ctx->type), ctx->type, (int) ctx->contentLength);
		}

		if (newdata || (ctx->remainingContent > 0 && in->length > 0)) {
			switch (ctx->type) {
			case FCGI_END_REQUEST:
				if (8 != ctx->contentLength) {
					ERROR(wrk->srv, "(%s) FastCGI end request message has unexpected length %i != 8",
						li_sockaddr_to_string(ctx->pool->config.sock_addr, wrk->tmp_str, TRUE)->str,
						(gint) ctx->contentLength);
					fastcgi_reset(ctx);
					return;
				}
				if (in->length < 8) return; /* wait for more */

				{
					unsigned char endreq[8];
					guint8 protocolStatus;

					if (!li_chunkqueue_extract_to_memory(in, 8, endreq, NULL)) abort();
					li_chunkqueue_skip(in, 8);
					ctx->remainingContent -= 8;

					protocolStatus = endreq[4];
					if (FCGI_REQUEST_COMPLETE != protocolStatus) {
						fastcgi_reset(ctx);
						return;
					}

					ctx->stdin_closed = TRUE;
					ctx->stdout_closed = TRUE;
					ctx->stderr_closed = TRUE;
					ctx->request_done = TRUE;
					ctx->fcgi_in.out->is_closed = TRUE;
					li_stream_notify_later(&ctx->fcgi_in);

					if (ctx->currentcon) {
						guint32 appStatus = (endreq[0] << 24) | (endreq[1] << 16) | (endreq[2] << 8) | endreq[3];
						const liFastCGIBackendCallbacks *callbacks = ctx->pool->callbacks;

						fcgi_debug("fastcgi end request: %i\n", appStatus);
						callbacks->end_request_cb(ctx->currentcon->vr, &ctx->pool->public, &ctx->currentcon->public, appStatus);
					}
				}
				break;
			case FCGI_STDOUT:
				if (0 == ctx->contentLength) {
					fcgi_debug("fastcgi stdout eof");
					ctx->stdout_closed = TRUE;
				} else if (ctx->stdout_closed) {
					fcgi_debug("fastcgi stdout data after eof");
					fastcgi_reset(ctx);
					return;
				} else {
					int len = MIN(in->length, ctx->remainingContent);
#ifdef FCGI_DEBUG
					GString *stdoutdata = g_string_new(0);
					li_chunkqueue_extract_to(in, len, stdoutdata, NULL);
					fcgi_debug("fastcgi stdout data: '%s'", stdoutdata->str);
					g_string_free(stdoutdata, TRUE);
#endif
					li_chunkqueue_steal_len(ctx->fcgi_in.out, in, len);
					ctx->remainingContent -= len;
				}
				li_stream_notify_later(&ctx->fcgi_in);
				break;
			case FCGI_STDERR:
				if (0 == ctx->contentLength) {
					ctx->stderr_closed = TRUE;
					break;
				}
				if (ctx->stderr_closed || NULL == ctx->currentcon) {
					fastcgi_reset(ctx);
					return;
				} else {
					gint len = ctx->remainingContent > in->length ? in->length : ctx->remainingContent;
					GString *errormsg = g_string_new(0);
					li_chunkqueue_extract_to(in, len, errormsg, NULL);
					li_chunkqueue_skip(in, len);
					ctx->remainingContent -= len;

					/* TODO: callback(errormsg) */
					g_string_free(errormsg, TRUE);
				}
				break;
			default:
				if (newdata) {
					WARNING(wrk->srv, "(%s) Unhandled fastcgi record type %i",
						li_sockaddr_to_string(ctx->pool->config.sock_addr, wrk->tmp_str, TRUE)->str,
						(gint) ctx->type);
				}
				{
					int len = li_chunkqueue_skip(in, ctx->remainingContent);
					ctx->remainingContent -= len;
				}
				break;
			}
		}

		if (0 == in->length || ctx->remainingContent > 0) return;

		if (ctx->remainingPadding > 0) {
			int len = li_chunkqueue_skip(in, ctx->remainingPadding);
			ctx->remainingPadding -= len;
		}
	}

	if (in->is_closed && !ctx->request_done) {
		if (0 != in->length || !ctx->stdout_closed) {
			fastcgi_reset(ctx);
		} else {
			ctx->stdin_closed = ctx->stdout_closed = ctx->stderr_closed = ctx->request_done = TRUE;
			ctx->fcgi_in.out->is_closed = TRUE;
			li_stream_simple_socket_close(ctx->iostream, FALSE);
		}
	}
}

/* fastcgi -> response body */
static void fastcgi_stream_in(liStream *stream, liStreamEvent event) {
	liFastCGIBackendContext *ctx = LI_CONTAINER_OF(stream, liFastCGIBackendContext, fcgi_in);
	fcgi_debug("fastcgi_stream_in event: %s\n", li_stream_event_string(event));
	switch (event) {
	case LI_STREAM_NEW_DATA:
		fastcgi_decode(ctx);
		break;
	case LI_STREAM_DISCONNECTED_SOURCE:
		if (!ctx->request_done) fastcgi_reset(ctx);
		break;
	case LI_STREAM_DISCONNECTED_DEST:
		if (!ctx->stdout_closed) {
			fastcgi_reset(ctx);
		} else {
			fastcgi_check_put(ctx);
		}
		break;
	case LI_STREAM_DESTROY:
		backend_ctx_unref(ctx);
	default:
		break;
	}
}


liFastCGIBackendPool* li_fastcgi_backend_pool_new(const liFastCGIBackendConfig *config) {
	liFastCGIBackendPool_p *pool = g_slice_new0(liFastCGIBackendPool_p);

	pool->config.callbacks = &backend_cbs;
	pool->config.sock_addr = li_sockaddr_dup(config->sock_addr);
	pool->config.max_connections = config->max_connections;
	pool->config.idle_timeout = config->idle_timeout;
	pool->config.connect_timeout = config->connect_timeout;
	pool->config.wait_timeout = config->wait_timeout;
	pool->config.disable_time = config->disable_time;
	pool->config.max_requests = config->max_requests;
	pool->config.watch_for_close = FALSE;

	pool->callbacks = config->callbacks;

	pool->public.subpool = li_backend_pool_new(&pool->config);

	return &pool->public;
}

void li_fastcgi_backend_pool_free(liFastCGIBackendPool *bpool) {
	li_backend_pool_free(bpool->subpool);
}

liBackendResult li_fastcgi_backend_get(liVRequest *vr, liFastCGIBackendPool *bpool, liFastCGIBackendConnection **pbcon, liFastCGIBackendWait **pbwait) {
	liFastCGIBackendPool_p *pool = LI_CONTAINER_OF(bpool, liFastCGIBackendPool_p, public);
	liBackendConnection *subcon = NULL;
	liBackendWait *subwait = (liBackendWait*) *pbwait;
	liBackendResult res;

	fcgi_debug("%s\n", "li_fastcgi_backend_get");

	res = li_backend_get(vr, pool->public.subpool, &subcon, &subwait);
	*pbwait = (liFastCGIBackendWait*) subwait;

	if (subcon != NULL) {
		liFastCGIBackendConnection_p *con = g_slice_new0(liFastCGIBackendConnection_p);
		liFastCGIBackendContext *ctx = subcon->data;
		liStream *http_out;

		assert(NULL != ctx);
		assert(LI_BACKEND_SUCCESS == res);
		con->ctx = ctx;
		con->vr = vr;
		ctx->currentcon = con;
		ctx->is_active = TRUE;
		*pbcon = &con->public;

		fcgi_debug("%s\n", "li_fastcgi_backend_get: got backend");

		assert(vr->wrk == li_worker_from_iostream(ctx->iostream));
		assert(vr->wrk == li_worker_from_stream(&ctx->fcgi_in));
		assert(vr->wrk == li_worker_from_stream(&ctx->fcgi_out));

		assert(li_event_active(&ctx->iostream->io_watcher));
		li_event_set_keep_loop_alive(&ctx->iostream->io_watcher, TRUE);

		assert(NULL != ctx->iostream);
		assert(-1 != li_event_io_fd(&ctx->iostream->io_watcher));

		assert(ctx->iostream->stream_in.dest == &ctx->fcgi_in);
		assert(ctx->iostream->stream_out.source == &ctx->fcgi_out);

		ctx->stdin_closed = ctx->stdout_closed = ctx->stderr_closed = ctx->request_done = FALSE;
		li_chunkqueue_reset(ctx->fcgi_in.out);

		stream_send_begin(ctx->fcgi_out.out, 1);
		fastcgi_send_env(vr, ctx->fcgi_out.out, 1);
		li_stream_notify_later(&ctx->fcgi_out);

		http_out = li_stream_http_response_handle(&ctx->fcgi_in, vr, TRUE, TRUE);

		li_vrequest_handle_indirect(vr, NULL);
		li_vrequest_indirect_connect(vr, &ctx->fcgi_out, http_out);

		li_stream_release(http_out);
	} else {
		*pbcon = NULL;
		assert(LI_BACKEND_SUCCESS != res);
		if (LI_BACKEND_WAIT == res) assert(NULL != subwait);

		fcgi_debug("%s\n", "li_fastcgi_backend_get: still waiting");
	}

	return res;
}

void li_fastcgi_backend_wait_stop(liVRequest *vr, liFastCGIBackendPool *bpool, liFastCGIBackendWait **pbwait) {
	liBackendWait *subwait = (liBackendWait*) *pbwait;
	*pbwait = NULL;

	li_backend_wait_stop(vr, bpool->subpool, &subwait);
}

void li_fastcgi_backend_put(liFastCGIBackendConnection *bcon) {
	liFastCGIBackendConnection_p *con = LI_CONTAINER_OF(bcon, liFastCGIBackendConnection_p, public);
	liFastCGIBackendContext *ctx = con->ctx;

	assert(NULL != ctx && con == ctx->currentcon);
	ctx->currentcon = NULL;
	con->ctx = NULL;
	con->vr = NULL;

	g_slice_free(liFastCGIBackendConnection_p, con);

	fastcgi_check_put(ctx);
}

/*
 * mod_deflate - compress content on the fly
 *
 * Author:
 *     Copyright (c) 2009 Stefan Bühler
 *     Copyright (c) 2010 Thomas Porzelt
 */

#include <lighttpd/base.h>
#include <lighttpd/plugin_core.h>

LI_API gboolean mod_deflate_init(liModules *mods, liModule *mod);
LI_API gboolean mod_deflate_free(liModules *mods, liModule *mod);

/* encoding names */
#define ENCODING_NAME_IDENTITY   "identity"
#define ENCODING_NAME_GZIP       "gzip"
#define ENCODING_NAME_X_GZIP     "x-gzip"
#define ENCODING_NAME_DEFLATE    "deflate"
#define ENCODING_NAME_COMPRESS   "compress"
#define ENCODING_NAME_BZIP2      "bzip2"
#define ENCODING_NAME_X_BZIP2    "x-bzip2"

typedef enum {
	ENCODING_IDENTITY,
	ENCODING_BZIP2,
	ENCODING_X_BZIP2,
	ENCODING_GZIP,
	ENCODING_X_GZIP,
	ENCODING_DEFLATE,
	ENCODING_COMPRESS
} encodings;

static const char* encoding_names[] = {
	"identity",
	"bzip2",
	"x-bzip2",
	"gzip",
	"x-gzip",
	"deflate",
	"compress",
	NULL
};

static const guint encoding_available_mask = 0
#ifdef HAVE_BZIP
	| (1 << ENCODING_BZIP2) | (1 << ENCODING_X_BZIP2)
#endif
#ifdef HAVE_ZLIB
	| (1 << ENCODING_GZIP) | (1 << ENCODING_X_GZIP) | (1 << ENCODING_DEFLATE)
#endif
;

typedef struct deflate_config deflate_config;
struct deflate_config {
	liPlugin *p;
	guint allowed_encodings;
	guint blocksize, output_buffer, compression_level;
};

/**********************************************************************************/

#ifdef HAVE_ZLIB

# include <zlib.h>

/* Copied gzip_header from apache 2.2's mod_deflate.c */
/* RFC 1952 Section 2.3 defines the gzip header:
 *
 * +---+---+---+---+---+---+---+---+---+---+
 * |ID1|ID2|CM |FLG|     MTIME     |XFL|OS |
 * +---+---+---+---+---+---+---+---+---+---+
 */
static const unsigned char gzip_header[] = {
	0x1f, 0x8b, Z_DEFLATED, 0,
	0, 0, 0, 0, /* mtime */
	0, 0x03 /* Unix OS_CODE */
};

typedef struct deflate_context_zlib deflate_context_zlib;
struct deflate_context_zlib {
	deflate_config conf;

	z_stream z;
	GByteArray *buf;
	gboolean is_gzip, gzip_header;
	unsigned long crc;
};

static void deflate_context_zlib_free(deflate_context_zlib *ctx) {
	z_stream *z;
	if (!ctx) return;

	z = &ctx->z;
	deflateEnd(z);

	g_byte_array_free(ctx->buf, TRUE);

	g_slice_free(deflate_context_zlib, ctx);
}

static deflate_context_zlib* deflate_context_zlib_create(liVRequest *vr, deflate_config *conf, gboolean is_gzip) {
	deflate_context_zlib *ctx = g_slice_new0(deflate_context_zlib);
	z_stream *z = &ctx->z;
	guint compression_level = conf->compression_level;
	guint window_size = -MAX_WBITS; /* suppress zlib-header */
	guint mem_level = 8;

	ctx->conf = *conf;

	z->zalloc = Z_NULL;
	z->zfree = Z_NULL;
	z->opaque = Z_NULL;
	z->total_in = 0;
	z->total_out = 0;
	z->next_out = NULL;
	z->avail_out = 0;

	if (Z_OK != deflateInit2(z,
			compression_level,
			Z_DEFLATED,
			window_size,
			mem_level,
			Z_DEFAULT_STRATEGY)) {
		g_slice_free(deflate_context_zlib, ctx);
		VR_ERROR(vr, "%s", "Couldn't init z_stream");
		return NULL;
	}

	ctx->buf = g_byte_array_new();
	g_byte_array_set_size(ctx->buf, conf->output_buffer);

	ctx->is_gzip = is_gzip;

	z->next_out = ctx->buf->data;
	z->avail_out = ctx->buf->len;

	return ctx;
}

static void deflate_filter_zlib_free(liVRequest *vr, liFilter *f) {
	deflate_context_zlib *ctx = (deflate_context_zlib*) f->param;
	UNUSED(vr);

	deflate_context_zlib_free(ctx);
}

static liHandlerResult deflate_filter_zlib(liVRequest *vr, liFilter *f) {
	deflate_context_zlib *ctx = (deflate_context_zlib*) f->param;
	const off_t blocksize = ctx->conf.blocksize;
	const off_t max_compress = 4 * blocksize;
	gboolean debug = (NULL != vr) && _OPTION(vr, ctx->conf.p, 0).boolean;
	z_stream *z = &ctx->z;
	off_t l = 0;
	liHandlerResult res;
	int rc;

	if (NULL == f->in) {
		f->out->is_closed = TRUE;
		return LI_HANDLER_GO_ON;
	}

	if (f->in->is_closed && 0 == f->in->length && f->out->is_closed) {
		/* nothing to do anymore */
		return LI_HANDLER_GO_ON;
	}

	if (f->out->is_closed) {
		li_chunkqueue_skip_all(f->in);
		li_stream_disconnect(&f->stream);
		if (debug) {
			VR_DEBUG(vr, "deflate out stream closed: in: %i, out : %i", (int) z->total_in, (int) z->total_out);
		}
		return LI_HANDLER_GO_ON;
	}

	if (ctx->is_gzip && !ctx->gzip_header) {
		ctx->gzip_header = TRUE;

		/* as the buffer is unused it really should be big enough */
		if (z->avail_out < sizeof(gzip_header)) {
			f->out->is_closed = TRUE;
			VR_ERROR(vr, "deflate error: %s", z->msg);
			return LI_HANDLER_ERROR;
		}

		/* copy gzip header into output buffer */
		memcpy(z->next_out, gzip_header, sizeof(gzip_header));

		/* initialize crc32 */
		ctx->crc = crc32(0L, Z_NULL, 0);
		z->next_out += sizeof(gzip_header);
		z->avail_out -= sizeof(gzip_header);
	}

	while (l < max_compress) {
		char *data;
		off_t len;
		liChunkIter ci;
		GError *err = NULL;

		if (0 == f->in->length) break;

		ci = li_chunkqueue_iter(f->in);

		if (LI_HANDLER_GO_ON != (res = li_chunkiter_read(ci, 0, blocksize, &data, &len, &err))) {
			if (NULL != err) {
				if (NULL != vr) VR_ERROR(vr, "Couldn't read data from chunkqueue: %s", err->message);
				g_error_free(err);
			}
			return res;
		}

		if (ctx->is_gzip) {
			ctx->crc = crc32(ctx->crc, (unsigned char*) data, len);
		}

		z->next_in = (unsigned char*) data;
		z->avail_in = len;

		do {
			if (Z_OK != deflate(z, Z_NO_FLUSH)) {
				f->out->is_closed = TRUE;
				if (NULL != vr) VR_ERROR(vr, "deflate error: %s", z->msg);
				return LI_HANDLER_ERROR;
			}

			if(z->avail_out == 0 || z->avail_in > 0) {
				li_chunkqueue_append_mem(f->out, ctx->buf->data, ctx->buf->len - z->avail_out);
				z->next_out = ctx->buf->data;
				z->avail_out = ctx->buf->len;
			}
		} while (z->avail_in > 0);

		li_chunkqueue_skip(f->in, len);
		l += len;
	}

	if (0 == f->in->length && f->in->is_closed) {
		do {
			rc = deflate(z, Z_FINISH);
			if (rc != Z_OK && rc != Z_STREAM_END) {
				f->out->is_closed = TRUE;
				if (NULL != vr) VR_ERROR(vr, "deflate error: %s", z->msg);
				return LI_HANDLER_ERROR;
			}

			/* flush every time until done */
			if (0 < ctx->buf->len - z->avail_out) {
				li_chunkqueue_append_mem(f->out, ctx->buf->data, ctx->buf->len - z->avail_out);
				z->next_out = ctx->buf->data;
				z->avail_out = ctx->buf->len;
			}
		} while (rc != Z_STREAM_END);

		if (ctx->is_gzip) {
			/* write gzip footer */
			unsigned char c[8];

			c[0] = (ctx->crc >>  0) & 0xff;
			c[1] = (ctx->crc >>  8) & 0xff;
			c[2] = (ctx->crc >> 16) & 0xff;
			c[3] = (ctx->crc >> 24) & 0xff;
			c[4] = (z->total_in >>  0) & 0xff;
			c[5] = (z->total_in >>  8) & 0xff;
			c[6] = (z->total_in >> 16) & 0xff;
			c[7] = (z->total_in >> 24) & 0xff;

			/* append footer to write_queue */
			li_chunkqueue_append_mem(f->out, c, 8);
		}

		if (debug) {
			VR_DEBUG(vr, "deflate finished: in: %i, out : %i", (int) z->total_in, (int) z->total_out);
		}

		f->out->is_closed = TRUE;
	}

	if (l > 0 && 0 == f->in->length && !f->in->is_closed) { /* flush z_stream */
		rc = deflate(z, Z_SYNC_FLUSH);
		if (rc != Z_OK && rc != Z_STREAM_END) {
			if (NULL != vr) VR_ERROR(vr, "deflate error: %s", z->msg);
			return LI_HANDLER_ERROR;
		}
	}

	/* flush output buffer if there is no more data pending */
	if (0 == f->in->length && 0 < ctx->buf->len - z->avail_out) {
		li_chunkqueue_append_mem(f->out, ctx->buf->data, ctx->buf->len - z->avail_out);
		z->next_out = ctx->buf->data;
		z->avail_out = ctx->buf->len;
	}

	return 0 == f->in->length ? LI_HANDLER_GO_ON : LI_HANDLER_COMEBACK;
}
#endif /* HAVE_ZLIB */

/**********************************************************************************/

#ifdef HAVE_BZIP

/* we don't need stdio interface */
# define BZ_NO_STDIO
# include <bzlib.h>


typedef struct deflate_context_bzip2 deflate_context_bzip2;
struct deflate_context_bzip2 {
	deflate_config conf;

	bz_stream bz;
	GByteArray *buf;
};

static void deflate_context_bzip2_free(deflate_context_bzip2 *ctx) {
	bz_stream *bz;
	if (!ctx) return;

	bz = &ctx->bz;
	BZ2_bzCompressEnd(bz);

	g_byte_array_free(ctx->buf, TRUE);

	g_slice_free(deflate_context_bzip2, ctx);
}

static deflate_context_bzip2* deflate_context_bzip2_create(liVRequest *vr, deflate_config *conf) {
	deflate_context_bzip2 *ctx = g_slice_new0(deflate_context_bzip2);
	bz_stream *bz = &ctx->bz;
	guint compression_level = conf->compression_level;

	ctx->conf = *conf;

	bz->bzalloc = NULL;
	bz->bzfree = NULL;
	bz->opaque = NULL;
	bz->total_in_lo32 = 0;
	bz->total_in_hi32 = 0;
	bz->total_out_lo32 = 0;
	bz->total_out_hi32 = 0;

	if (BZ_OK != BZ2_bzCompressInit(bz, compression_level /* blocksize */, 0 /* no output */, 30 /* workFactor: default */)) {
		VR_ERROR(vr, "%s", "Couldn't init bz_stream");
		g_slice_free(deflate_context_bzip2, ctx);
		return NULL;
	}

	ctx->buf = g_byte_array_new();
	g_byte_array_set_size(ctx->buf, conf->output_buffer);

	bz->next_out = (char*) ctx->buf->data;
	bz->avail_out = ctx->buf->len;

	return ctx;
}

static void deflate_filter_bzip2_free(liVRequest *vr, liFilter *f) {
	deflate_context_bzip2 *ctx = (deflate_context_bzip2*) f->param;
	UNUSED(vr);

	deflate_context_bzip2_free(ctx);
}

static liHandlerResult deflate_filter_bzip2(liVRequest *vr, liFilter *f) {
	deflate_context_bzip2 *ctx = (deflate_context_bzip2*) f->param;
	const off_t blocksize = ctx->conf.blocksize;
	const off_t max_compress = 4 * blocksize;
	gboolean debug = (NULL != vr) && _OPTION(vr, ctx->conf.p, 0).boolean;
	bz_stream *bz = &ctx->bz;
	off_t l = 0;
	liHandlerResult res;
	int rc;

	if (NULL == f->in) {
		/* didn't handle f->in->is_closed? abort forwarding */
		if (!f->out->is_closed) li_stream_reset(&f->stream);
		return LI_HANDLER_GO_ON;
	}

	if (f->in->is_closed && 0 == f->in->length && f->out->is_closed) {
		/* nothing to do anymore */
		return LI_HANDLER_GO_ON;
	}

	if (f->out->is_closed) {
		li_chunkqueue_skip_all(f->in);
		li_stream_disconnect(&f->stream);
		if (debug) {
			VR_DEBUG(vr, "deflate out stream closed: in: %i, out : %i", (int) bz->total_in_lo32, (int) bz->total_out_lo32);
		}
		return LI_HANDLER_GO_ON;
	}

	while (l < max_compress) {
		char *data;
		off_t len;
		liChunkIter ci;
		GError *err = NULL;

		if (0 == f->in->length) break;

		ci = li_chunkqueue_iter(f->in);

		if (LI_HANDLER_GO_ON != (res = li_chunkiter_read(ci, 0, blocksize, &data, &len, &err))) {
			if (NULL != err) {
				if (NULL != vr) VR_ERROR(vr, "Couldn't read data from chunkqueue: %s", err->message);
				g_error_free(err);
			}
			return res;
		}

		bz->next_in = data;
		bz->avail_in = len;

		do {
			rc = BZ2_bzCompress(bz, BZ_RUN);
			if (rc != BZ_RUN_OK) {
				f->out->is_closed = TRUE;
				if (NULL != vr) VR_ERROR(vr, "BZ2_bzCompress error: rc = %i", rc);
				return LI_HANDLER_ERROR;
			}

			if(bz->avail_out == 0 || bz->avail_in > 0) {
				li_chunkqueue_append_mem(f->out, ctx->buf->data, ctx->buf->len - bz->avail_out);
				bz->next_out = (char*) ctx->buf->data;
				bz->avail_out = ctx->buf->len;
			}
		} while (bz->avail_in > 0);

		li_chunkqueue_skip(f->in, len);
		l += len;
	}

	if (0 == f->in->length && f->in->is_closed) {
		do {
			rc = BZ2_bzCompress(bz, BZ_FINISH);
			if (rc != BZ_RUN_OK && rc != BZ_STREAM_END && rc != BZ_FINISH_OK) {
				f->out->is_closed = TRUE;
				if (NULL != vr) VR_ERROR(vr, "BZ2_bzCompress error: rc = %i", rc);
				return LI_HANDLER_ERROR;
			}

			/* flush every time until done */
			if (0 < ctx->buf->len - bz->avail_out) {
				li_chunkqueue_append_mem(f->out, ctx->buf->data, ctx->buf->len - bz->avail_out);
				bz->next_out = (char*) ctx->buf->data;
				bz->avail_out = ctx->buf->len;
			}
		} while (rc == BZ_RUN_OK || rc == BZ_FINISH_OK);

		if (debug) {
			VR_DEBUG(vr, "deflate finished: in: %i, out : %i", (int) bz->total_in_lo32, (int) bz->total_out_lo32);
		}

		f->out->is_closed = TRUE;
	}

	/* flush output buffer if there is no more data pending */
	if (0 == f->in->length && 0 < ctx->buf->len - bz->avail_out) {
		li_chunkqueue_append_mem(f->out, ctx->buf->data, ctx->buf->len - bz->avail_out);
		bz->next_out = (char*) ctx->buf->data;
		bz->avail_out = ctx->buf->len;
	}

	return 0 == f->in->length ? LI_HANDLER_GO_ON : LI_HANDLER_COMEBACK;
}
#endif /* HAVE_BZIP */

static liHandlerResult deflate_filter_null(liVRequest *vr, liFilter *f) {
	UNUSED(vr);
	if (NULL != f->in) {
		li_chunkqueue_skip_all(f->in);
		li_stream_disconnect(&f->stream);
	}
	return LI_HANDLER_GO_ON;
}

/**********************************************************************************/

/* returns TRUE if handled with 304, FALSE otherwise */
static gboolean cached_handle_etag(liVRequest *vr, gboolean debug, liHttpHeader *hh_etag, const char* enc_name) {
	GString *s = vr->wrk->tmp_str;

	if (!hh_etag) return FALSE;

	g_string_truncate(s, 0);
	g_string_append_len(s, LI_HEADER_VALUE_LEN(hh_etag));
	g_string_append_len(s, CONST_STR_LEN("-"));
	g_string_append_len(s, enc_name, strlen(enc_name));
	li_etag_mutate(s, s);
	g_string_truncate(hh_etag->data, hh_etag->keylen + 2);
	g_string_append_len(hh_etag->data, GSTR_LEN(s));

	if (200 == vr->response.http_status && li_http_response_handle_cachable(vr)) {
		if (debug || CORE_OPTION(LI_CORE_OPTION_DEBUG_REQUEST_HANDLING).boolean) {
			VR_DEBUG(vr, "%s", "deflate: etag handling => 304 Not Modified");
		}
		vr->response.http_status = 304;
		return TRUE;
	}
	return FALSE;
}

static guint header_to_encoding_mask(const gchar *s) {
	guint encoding_mask = 0, i;

	for (i = 1; encoding_names[i]; i++) {
		const gchar *p = strstr(s, encoding_names[i]);
		if (NULL != p && (p == s || p[-1] == ' ' || p[-1] == ',')) {
			encoding_mask |= 1 << i;
		}
	}

	return encoding_mask;
}

static liHandlerResult deflate_handle(liVRequest *vr, gpointer param, gpointer *context) {
	deflate_config *config = (deflate_config*) param;
	GList *hh_encoding_entry, *hh_etag_entry;
	liHttpHeader *hh_encoding, *hh_etag = NULL;
	guint encoding_mask = 0, i;
	gboolean debug = _OPTION(vr, config->p, 0).boolean;
	gboolean is_head_request = (vr->request.http_method == LI_HTTP_METHOD_HEAD);

	UNUSED(context);

	LI_VREQUEST_WAIT_FOR_RESPONSE_HEADERS(vr);

	/* disable compression for some http status types. */
	switch(vr->response.http_status) {
	case 100:
	case 101:
	case 204:
	case 205:
	case 206:
	case 304:
		/* disable compression as we have no response entity */
		return LI_HANDLER_GO_ON;
	default:
		break;
	}

	/* response already encoded */
	if (li_http_header_find_first(vr->response.headers, CONST_STR_LEN("content-encoding"))) {
		if (debug) {
			VR_DEBUG(vr, "%s", "deflate: Content-Encoding already set => not compressing");
		}
		return LI_HANDLER_GO_ON;
	}

	/* don't mess with content after transfer-encoding */
	if (li_http_header_find_first(vr->response.headers, CONST_STR_LEN("transfer-encoding"))) {
		if (debug) {
			VR_DEBUG(vr, "%s", "deflate: Transfer-Encoding set => not compressing");
		}
		return LI_HANDLER_GO_ON;
	}

	/* announce that we have looked for accept-encoding */
	li_http_header_append(vr->response.headers, CONST_STR_LEN("Vary"), CONST_STR_LEN("Accept-Encoding"));

	hh_encoding_entry = li_http_header_find_first(vr->request.headers, CONST_STR_LEN("accept-encoding"));
	while (hh_encoding_entry) {
		hh_encoding = (liHttpHeader*) hh_encoding_entry->data;
		encoding_mask |= header_to_encoding_mask(LI_HEADER_VALUE(hh_encoding));
		hh_encoding_entry = li_http_header_find_next(hh_encoding_entry, CONST_STR_LEN("accept-encoding"));
	}

	if (0 == encoding_mask)
		return LI_HANDLER_GO_ON; /* no known encoding found */

	encoding_mask &= encoding_available_mask & config->allowed_encodings;
	if (0 == encoding_mask) {
		if (debug) {
			VR_DEBUG(vr, "%s", "no common encoding found => not compressing");
		}
		return LI_HANDLER_GO_ON; /* no common encoding found */
	}

	/* find best encoding (first in list) */
	for (i = 1; 0 == (encoding_mask & (1 << i)) ; i++) ;

	hh_etag_entry = li_http_header_find_first(vr->response.headers, CONST_STR_LEN("etag"));
	if (hh_etag_entry) {
		if (li_http_header_find_next(hh_etag_entry, CONST_STR_LEN("etag"))) {
			if (debug || CORE_OPTION(LI_CORE_OPTION_DEBUG_REQUEST_HANDLING).boolean) {
				VR_DEBUG(vr, "%s", "duplicate etag header in response, will not deflate it");
			}
			return LI_HANDLER_GO_ON;
		}
		hh_etag = (liHttpHeader*) hh_etag_entry->data;
	}

	if (debug || CORE_OPTION(LI_CORE_OPTION_DEBUG_REQUEST_HANDLING).boolean) {
		VR_DEBUG(vr, "deflate: compressing using %s encoding", encoding_names[i]);
	}

	switch ((encodings) i) {
	case ENCODING_IDENTITY:
		return LI_HANDLER_GO_ON;
	case ENCODING_BZIP2:
	case ENCODING_X_BZIP2:
#ifdef HAVE_BZIP
		if (cached_handle_etag(vr, debug, hh_etag, encoding_names[i])) return LI_HANDLER_GO_ON;
		if (!is_head_request) {
			deflate_context_bzip2 *ctx;
			ctx = deflate_context_bzip2_create(vr, config);
			if (!ctx) return LI_HANDLER_GO_ON;
			li_vrequest_add_filter_out(vr, deflate_filter_bzip2, deflate_filter_bzip2_free, NULL, ctx);
		}
		break;
#endif
		return LI_HANDLER_GO_ON;
	case ENCODING_GZIP:
	case ENCODING_X_GZIP:
#ifdef HAVE_ZLIB
		if (cached_handle_etag(vr, debug, hh_etag, encoding_names[i])) return LI_HANDLER_GO_ON;
		if (!is_head_request) {
			deflate_context_zlib *ctx;
			ctx = deflate_context_zlib_create(vr, config, TRUE);
			if (!ctx) return LI_HANDLER_GO_ON;
			li_vrequest_add_filter_out(vr, deflate_filter_zlib, deflate_filter_zlib_free, NULL, ctx);
		}
		break;
#endif
		return LI_HANDLER_GO_ON;
	case ENCODING_DEFLATE:
#ifdef HAVE_ZLIB
		if (cached_handle_etag(vr, debug, hh_etag, encoding_names[i])) return LI_HANDLER_GO_ON;
		if (!is_head_request) {
			deflate_context_zlib *ctx;
			ctx = deflate_context_zlib_create(vr, config, FALSE);
			if (!ctx) return LI_HANDLER_GO_ON;
			li_vrequest_add_filter_out(vr, deflate_filter_zlib, deflate_filter_zlib_free, NULL, ctx);
		}
		break;
#endif
		return LI_HANDLER_GO_ON;
	case ENCODING_COMPRESS:
		return LI_HANDLER_GO_ON;
	}

	if (is_head_request) {
		/* kill content so response.c doesn't send wrong content-length */
		liFilter *f = li_vrequest_add_filter_out(vr, deflate_filter_null, NULL, NULL, NULL);
		f->out->is_closed = TRUE;
	}

	li_http_header_insert(vr->response.headers, CONST_STR_LEN("Content-Encoding"), encoding_names[i], strlen(encoding_names[i]));
	li_http_header_remove(vr->response.headers, CONST_STR_LEN("content-length"));

	return LI_HANDLER_GO_ON;
}

static void deflate_free(liServer *srv, gpointer param) {
	deflate_config *conf = (deflate_config*) param;
	UNUSED(srv);

	g_slice_free(deflate_config, conf);
}

/* deflate option names */
static const GString
	don_encodings = { CONST_STR_LEN("encodings"), 0 },
	don_blocksize = { CONST_STR_LEN("blocksize"), 0 },
	don_outputbuffer = { CONST_STR_LEN("output-buffer"), 0 },
	don_compression_level = { CONST_STR_LEN("compression-level"), 0 }
;

static liAction* deflate_create(liServer *srv, liWorker *wrk, liPlugin* p, liValue *val, gpointer userdata) {
	deflate_config *conf;
	gboolean
		have_encodings_parameter = FALSE,
		have_blocksize_parameter = FALSE,
		have_outputbuffer_parameter = FALSE,
		have_compression_level_parameter = FALSE;
	UNUSED(wrk); UNUSED(userdata);

	val = li_value_get_single_argument(val);

	if (NULL != val && NULL == (val = li_value_to_key_value_list(val))) {
		ERROR(srv, "%s", "deflate expects a optional hash/key-value list as parameter");
		return NULL;
	}

	conf = g_slice_new0(deflate_config);
	conf->p = p;
	conf->allowed_encodings = encoding_available_mask;
	conf->blocksize = 16*1024;
	conf->output_buffer = 4*1024;
	conf->compression_level = 1;

	LI_VALUE_FOREACH(entry, val)
		liValue *entryKey = li_value_list_at(entry, 0);
		liValue *entryValue = li_value_list_at(entry, 1);
		GString *entryKeyStr;

		if (LI_VALUE_STRING != li_value_type(entryKey)) {
			ERROR(srv, "%s", "deflate doesn't take default keys");
			goto option_failed;
		}
		entryKeyStr = entryKey->data.string; /* keys are either NONE or STRING */

		if (g_string_equal(entryKeyStr, &don_encodings)) {
			if (LI_VALUE_STRING != li_value_type(entryValue)) {
				ERROR(srv, "deflate option '%s' expects string as parameter", entryKeyStr->str);
				goto option_failed;
			}
			if (have_encodings_parameter) {
				ERROR(srv, "duplicate deflate option '%s'", entryKeyStr->str);
				goto option_failed;
			}
			have_encodings_parameter = TRUE;
			conf->allowed_encodings = header_to_encoding_mask(entryValue->data.string->str);
		} else if (g_string_equal(entryKeyStr, &don_blocksize)) {
			if (LI_VALUE_NUMBER != li_value_type(entryValue) || entryValue->data.number <= 0) {
				ERROR(srv, "deflate option '%s' expects positive integer as parameter", entryKeyStr->str);
				goto option_failed;
			}
			if (have_blocksize_parameter) {
				ERROR(srv, "duplicate deflate option '%s'", entryKeyStr->str);
				goto option_failed;
			}
			have_blocksize_parameter = TRUE;
			conf->blocksize = entryValue->data.number;
		} else if (g_string_equal(entryKeyStr, &don_outputbuffer)) {
			if (LI_VALUE_NUMBER != li_value_type(entryValue) || entryValue->data.number <= 0) {
				ERROR(srv, "deflate option '%s' expects positive integer as parameter", entryKeyStr->str);
				goto option_failed;
			}
			if (have_outputbuffer_parameter) {
				ERROR(srv, "duplicate deflate option '%s'", entryKeyStr->str);
				goto option_failed;
			}
			have_outputbuffer_parameter = TRUE;
			conf->output_buffer = entryValue->data.number;
		} else if (g_string_equal(entryKeyStr, &don_compression_level)) {
			if (LI_VALUE_NUMBER != li_value_type(entryValue) || entryValue->data.number <= 0 || entryValue->data.number > 9) {
				ERROR(srv, "deflate option '%s' expects an integer between 1 and 9 as parameter", entryKeyStr->str);
				goto option_failed;
			}
			if (have_compression_level_parameter) {
				ERROR(srv, "duplicate deflate option '%s'", entryKeyStr->str);
				goto option_failed;
			}
			have_compression_level_parameter = TRUE;
			conf->compression_level = entryValue->data.number;
		} else {
			ERROR(srv, "unknown option for deflate '%s'", entryKeyStr->str);
			goto option_failed;
		}
	LI_VALUE_END_FOREACH()

	return li_action_new_function(deflate_handle, NULL, deflate_free, conf);

option_failed:
	g_slice_free(deflate_config, conf);
	return NULL;
}

static const liPluginOption options[] = {
	{ "deflate.debug", LI_VALUE_BOOLEAN, FALSE, NULL },

	{ NULL, 0, 0, NULL }
};

static const liPluginAction actions[] = {
	{ "deflate", deflate_create, NULL },
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

gboolean mod_deflate_init(liModules *mods, liModule *mod) {
	MODULE_VERSION_CHECK(mods);

	mod->config = li_plugin_register(mods->main, "mod_deflate", plugin_init, NULL);

	return mod->config != NULL;
}

gboolean mod_deflate_free(liModules *mods, liModule *mod) {
	if (mod->config)
		li_plugin_free(mods->main, mod->config);

	return TRUE;
}

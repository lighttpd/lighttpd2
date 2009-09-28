/*
 * mod_deflate - compress content on the fly
 *
 * Description:
 *     compress content on the fly
 *
 *     Does not compress:
 *      - HEAD requests
 *      - response status: 100, 101, 204, 205, 304
 *      - already compressed content
 *      - if more than one etag response header is sent
 *      - if no common encoding is found
 *
 *     Supported encodings
 *      - gzip, deflate (needs zlib)
 *      - bzip2 (needs bzip2)
 *
 *     + Modifies etag response header (if present)
 *     + Adds "Vary: Accept-Encoding" response header
 *     + Resets Content-Length header
 *
 * Setups:
 *     none
 *
 * Options:
 *     deflate.debug <boolean>
 *
 * Actions:
 *     deflate
 *
 * Example config:
 *     deflate;
 *
 * TODO:
 *     - etag 304 handling
 *
 * Author:
 *     Copyright (c) 2009 Stefan Bühler
 */

#include <lighttpd/base.h>
#include <lighttpd/plugin_core.h>

LI_API gboolean mod_deflate_init(liModules *mods, liModule *mod);
LI_API gboolean mod_deflate_free(liModules *mods, liModule *mod);

/* encoding names */
#define ENCODING_NAME_IDENTITY   "identity"
#define ENCODING_NAME_GZIP       "gzip"
#define ENCODING_NAME_DEFLATE    "deflate"
#define ENCODING_NAME_COMPRESS   "compress"
#define ENCODING_NAME_BZIP2      "bzip2"

typedef enum {
	ENCODING_IDENTITY,
	ENCODING_BZIP2,
	ENCODING_GZIP,
	ENCODING_DEFLATE,
	ENCODING_COMPRESS
} encodings;

static const char* encoding_names[] = {
	"identity",
	"bzip2",
	"gzip",
	"deflate",
	"compress",
	NULL
};

static const guint encoding_available_mask = 0
#ifdef HAVE_BZIP
	| (1 << ENCODING_BZIP2)
#endif
#ifdef HAVE_ZLIB
	| (1 << ENCODING_GZIP) | (1 << ENCODING_DEFLATE)
#endif
;

typedef struct deflate_config deflate_config;
struct deflate_config {
	liPlugin *p;
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
	liPlugin *p;
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

static deflate_context_zlib* deflate_context_zlib_create(liVRequest *vr, liPlugin *p, gboolean is_gzip) {
	deflate_context_zlib *ctx = g_slice_new0(deflate_context_zlib);
	z_stream *z = &ctx->z;
	guint compression_level = Z_DEFAULT_COMPRESSION;
	guint window_size = -MAX_WBITS; /* supress zlib-header */
	guint mem_level = 8;

	ctx->p = p;

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
	g_byte_array_set_size(ctx->buf, 64*1024);

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
	const off_t blocksize = 64*1024;
	const off_t max_compress = 4 * blocksize;

	deflate_context_zlib *ctx = (deflate_context_zlib*) f->param;
	gboolean debug = _OPTION(vr, ctx->p, 0).boolean;
	z_stream *z = &ctx->z;
	off_t l = 0;
	liHandlerResult res;
	int rc;
	UNUSED(vr);

	if (f->in->is_closed && 0 == f->in->length && f->out->is_closed) {
		/* nothing to do anymore */
		return LI_HANDLER_GO_ON;
	}

	if (f->out->is_closed) {
		li_chunkqueue_skip_all(f->in);
		f->in->is_closed = TRUE;
		if (debug) {
			VR_DEBUG(vr, "deflate: %s", "connection closed by remote");
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

		if (0 == f->in->length) break;

		ci = li_chunkqueue_iter(f->in);

		if (LI_HANDLER_GO_ON != (res = li_chunkiter_read(vr, ci, 0, blocksize, &data, &len)))
			return res;

		if (ctx->is_gzip) {
			ctx->crc = crc32(ctx->crc, (unsigned char*) data, len);
		}

		z->next_in = (unsigned char*) data;
		z->avail_in = len;

		do {
			if (Z_OK != deflate(z, Z_NO_FLUSH)) {
				f->out->is_closed = TRUE;
				VR_ERROR(vr, "deflate error: %s", z->msg);
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
				VR_ERROR(vr, "deflate error: %s", z->msg);
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
			f->out->is_closed = TRUE;
			VR_ERROR(vr, "deflate error: %s", z->msg);
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
	liPlugin *p;
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

static deflate_context_bzip2* deflate_context_bzip2_create(liVRequest *vr, liPlugin *p) {
	deflate_context_bzip2 *ctx = g_slice_new0(deflate_context_bzip2);
	bz_stream *bz = &ctx->bz;
	guint compression_level = 9;

	ctx->p = p;

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
	g_byte_array_set_size(ctx->buf, 64*1024);

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
	const off_t blocksize = 64*1024;
	const off_t max_compress = 4 * blocksize;

	deflate_context_bzip2 *ctx = (deflate_context_bzip2*) f->param;
	gboolean debug = _OPTION(vr, ctx->p, 0).boolean;
	bz_stream *bz = &ctx->bz;
	off_t l = 0;
	liHandlerResult res;
	int rc;
	UNUSED(vr);

	if (f->in->is_closed && 0 == f->in->length && f->out->is_closed) {
		/* nothing to do anymore */
		return LI_HANDLER_GO_ON;
	}

	if (f->out->is_closed) {
		li_chunkqueue_skip_all(f->in);
		f->in->is_closed = TRUE;
		if (debug) {
			VR_DEBUG(vr, "deflate: %s", "connection closed by remote");
		}
		return LI_HANDLER_GO_ON;
	}

	while (l < max_compress) {
		char *data;
		off_t len;
		liChunkIter ci;

		if (0 == f->in->length) break;

		ci = li_chunkqueue_iter(f->in);

		if (LI_HANDLER_GO_ON != (res = li_chunkiter_read(vr, ci, 0, blocksize, &data, &len)))
			return res;

		bz->next_in = data;
		bz->avail_in = len;

		do {
			rc = BZ2_bzCompress(bz, BZ_RUN);
			if (rc != BZ_RUN_OK) {
				f->out->is_closed = TRUE;
				VR_ERROR(vr, "BZ2_bzCompress error: rc = %i", rc);
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
			if (rc != BZ_RUN_OK && rc != BZ_STREAM_END) {
				f->out->is_closed = TRUE;
				VR_ERROR(vr, "BZ2_bzCompress error: rc = %i", rc);
				return LI_HANDLER_ERROR;
			}

			/* flush every time until done */
			if (0 < ctx->buf->len - bz->avail_out) {
				li_chunkqueue_append_mem(f->out, ctx->buf->data, ctx->buf->len - bz->avail_out);
				bz->next_out = (char*) ctx->buf->data;
				bz->avail_out = ctx->buf->len;
			}
		} while (rc != BZ_STREAM_END);

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

/**********************************************************************************/

static liHandlerResult deflate_handle(liVRequest *vr, gpointer param, gpointer *context) {
	deflate_config *config = (deflate_config*) param;
	GList *hh_encoding_entry, *hh_etag_entry;
	liHttpHeader *hh_encoding, *hh_etag = NULL;
	guint encoding_mask = 0, i;
	gboolean debug = _OPTION(vr, config->p, 0).boolean;

	UNUSED(context);

	if (vr->request.http_method == LI_HTTP_METHOD_HEAD) {
		if (debug) {
			VR_DEBUG(vr, "%s", "deflate: method = HEAD => not compressing");
		}
		return LI_HANDLER_GO_ON;
	}

	VREQUEST_WAIT_FOR_RESPONSE_HEADERS(vr);

	/* disable compression for some http status types. */
	switch(vr->response.http_status) {
	case 100:
	case 101:
	case 204:
	case 205:
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

	hh_encoding_entry = li_http_header_find_first(vr->request.headers, CONST_STR_LEN("accept-encoding"));
	while (hh_encoding_entry) {
		hh_encoding = (liHttpHeader*) hh_encoding_entry->data;
		for (i = 1; encoding_names[i]; i++) {
			if (strstr(HEADER_VALUE(hh_encoding), encoding_names[i])) {
				encoding_mask |= 1 << i;
			}
		}

		hh_encoding_entry = li_http_header_find_next(hh_encoding_entry, CONST_STR_LEN("accept-encoding"));
	}

	if (0 == encoding_mask)
		return LI_HANDLER_GO_ON; /* no known encoding found */

	encoding_mask &= encoding_available_mask;
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
#ifdef HAVE_BZIP
		{
			deflate_context_bzip2 *ctx = deflate_context_bzip2_create(vr, config->p);
			if (!ctx) return LI_HANDLER_GO_ON;
			li_vrequest_add_filter_out(vr, deflate_filter_bzip2, deflate_filter_bzip2_free, ctx);
		}
		break;
#endif
		return LI_HANDLER_GO_ON;
	case ENCODING_GZIP:
#ifdef HAVE_ZLIB
		{
			deflate_context_zlib *ctx = deflate_context_zlib_create(vr, config->p, TRUE);
			if (!ctx) return LI_HANDLER_GO_ON;
			li_vrequest_add_filter_out(vr, deflate_filter_zlib, deflate_filter_zlib_free, ctx);
		}
		break;
#endif
		return LI_HANDLER_GO_ON;
	case ENCODING_DEFLATE:
#ifdef HAVE_ZLIB
		{
			deflate_context_zlib *ctx = deflate_context_zlib_create(vr, config->p, FALSE);
			if (!ctx) return LI_HANDLER_GO_ON;
			li_vrequest_add_filter_out(vr, deflate_filter_zlib, deflate_filter_zlib_free, ctx);
		}
		break;
#endif
		return LI_HANDLER_GO_ON;
	case ENCODING_COMPRESS:
		return LI_HANDLER_GO_ON;
	}

	li_http_header_insert(vr->response.headers, CONST_STR_LEN("Content-Encoding"), encoding_names[i], strlen(encoding_names[i]));
	li_http_header_append(vr->response.headers, CONST_STR_LEN("Vary"), CONST_STR_LEN("Accept-Encoding"));
	li_http_header_remove(vr->response.headers, CONST_STR_LEN("content-length"));
	if (hh_etag) {
		GString *s = vr->wrk->tmp_str;
		g_string_truncate(s, 0);
		g_string_append_len(s, HEADER_VALUE_LEN(hh_etag));
		g_string_append_len(s, CONST_STR_LEN("-"));
		g_string_append_len(s, encoding_names[i], strlen(encoding_names[i]));
		li_etag_mutate(s, s);
		g_string_truncate(hh_etag->data, hh_etag->keylen + 2);
		g_string_append_len(hh_etag->data, GSTR_LEN(s));
	}

	return LI_HANDLER_GO_ON;
}

static void deflate_free(liServer *srv, gpointer param) {
	deflate_config *conf = (deflate_config*) param;
	UNUSED(srv);

	g_slice_free(deflate_config, conf);
}

static liAction* deflate_create(liServer *srv, liPlugin* p, liValue *val) {
	deflate_config *conf;
	UNUSED(srv);
	UNUSED(p);
	UNUSED(val);

	conf = g_slice_new0(deflate_config);
	conf->p = p;

	return li_action_new_function(deflate_handle, NULL, deflate_free, conf);
}

static const liPluginOption options[] = {
	{ "deflate.debug", LI_VALUE_BOOLEAN, NULL, NULL, NULL },

	{ NULL, 0, NULL, NULL, NULL }
};

static const liPluginAction actions[] = {
	{ "deflate", deflate_create },
	{ NULL, NULL }
};

static const liPluginSetup setups[] = {
	{ NULL, NULL }
};


static void plugin_init(liServer *srv, liPlugin *p) {
	UNUSED(srv);

	p->options = options;
	p->actions = actions;
	p->setups = setups;
}

gboolean mod_deflate_init(liModules *mods, liModule *mod) {
	MODULE_VERSION_CHECK(mods);

	mod->config = li_plugin_register(mods->main, "mod_deflate", plugin_init);

	return mod->config != NULL;
}

gboolean mod_deflate_free(liModules *mods, liModule *mod) {
	if (mod->config)
		li_plugin_free(mods->main, mod->config);

	return TRUE;
}

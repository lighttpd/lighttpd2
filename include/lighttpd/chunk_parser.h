#ifndef _LIGHTTPD_CHUNK_PARSER_H_
#define _LIGHTTPD_CHUNK_PARSER_H_

#ifndef _LIGHTTPD_BASE_H_
#error Please include <lighttpd/base.h> instead of this file
#endif

struct liChunkParserCtx {
	liChunkQueue *cq;

	goffset bytes_in;

	/* current position
	 * buf is curi[start..start+length)
	 */
	liChunkIter curi;
	off_t start, length;
	char *buf;

	int cs;
};

struct liChunkParserMark {
	liChunkIter ci;
	off_t pos, abs_pos;
};

LI_API void li_chunk_parser_init(liChunkParserCtx *ctx, liChunkQueue *cq);
LI_API void li_chunk_parser_reset(liChunkParserCtx *ctx);
LI_API liHandlerResult li_chunk_parser_prepare(liChunkParserCtx *ctx);
LI_API liHandlerResult li_chunk_parser_next(liChunkParserCtx *ctx, char **p, char **pe, GError **err);
LI_API void li_chunk_parser_done(liChunkParserCtx *ctx, goffset len);

/* extract [from..to) */
LI_API gboolean li_chunk_extract_to(liChunkParserMark from, liChunkParserMark to, GString *dest, GError **err);
LI_API GString* li_chunk_extract(liChunkParserMark from, liChunkParserMark to, GError **err);

INLINE liChunkParserMark li_chunk_parser_getmark(liChunkParserCtx *ctx, const char *fpc);

/********************
 * Inline functions *
 ********************/

INLINE liChunkParserMark li_chunk_parser_getmark(liChunkParserCtx *ctx, const char *fpc) {
	liChunkParserMark m;
	m.ci = ctx->curi;
	m.pos = ctx->start + fpc - ctx->buf;
	m.abs_pos = ctx->bytes_in + fpc - ctx->buf;
	return m;
}

#define LI_GETMARK(FPC) (li_chunk_parser_getmark(&ctx->chunk_ctx, FPC))

#endif

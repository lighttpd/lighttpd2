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
	off_t pos;
};

LI_API void chunk_parser_init(liChunkParserCtx *ctx, liChunkQueue *cq);
LI_API void chunk_parser_reset(liChunkParserCtx *ctx);
LI_API liHandlerResult chunk_parser_prepare(liChunkParserCtx *ctx);
LI_API liHandlerResult chunk_parser_next(liVRequest *vr, liChunkParserCtx *ctx, char **p, char **pe);
LI_API void chunk_parser_done(liChunkParserCtx *ctx, goffset len);

LI_API gboolean chunk_extract_to(liVRequest *vr, liChunkParserMark from, liChunkParserMark to, GString *dest);
LI_API GString* chunk_extract(liVRequest *vr, liChunkParserMark from, liChunkParserMark to);

INLINE liChunkParserMark chunk_parser_getmark(liChunkParserCtx *ctx, const char *fpc);

/********************
 * Inline functions *
 ********************/

INLINE liChunkParserMark chunk_parser_getmark(liChunkParserCtx *ctx, const char *fpc) {
	liChunkParserMark m;
	m.ci = ctx->curi;
	m.pos = ctx->start + fpc - ctx->buf;
	return m;
}

#define GETMARK(FPC) (chunk_parser_getmark(&ctx->chunk_ctx, FPC))

#endif

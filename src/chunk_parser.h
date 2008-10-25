#ifndef _LIGHTTPD_CHUNK_PARSER_H_
#define _LIGHTTPD_CHUNK_PARSER_H_

struct chunk_parser_ctx;
typedef struct chunk_parser_ctx chunk_parser_ctx;

struct chunk_parser_mark;
typedef struct chunk_parser_mark chunk_parser_mark;

#include "chunk.h"

struct chunk_parser_ctx {
	chunkqueue *cq;

	goffset bytes_in;

	/* current position
	 * buf is curi[start..start+length)
	 */
	chunkiter curi;
	off_t start, length;
	char *buf;

	int cs;
};

struct chunk_parser_mark {
	chunkiter ci;
	off_t pos;
};

LI_API void chunk_parser_init(chunk_parser_ctx *ctx, chunkqueue *cq);
LI_API void chunk_parser_reset(chunk_parser_ctx *ctx);
LI_API handler_t chunk_parser_prepare(chunk_parser_ctx *ctx);
LI_API handler_t chunk_parser_next(struct vrequest *vr, chunk_parser_ctx *ctx, char **p, char **pe);
LI_API void chunk_parser_done(chunk_parser_ctx *ctx, goffset len);

LI_API gboolean chunk_extract_to(struct vrequest *vr, chunk_parser_mark from, chunk_parser_mark to, GString *dest);
LI_API GString* chunk_extract(struct vrequest *vr, chunk_parser_mark from, chunk_parser_mark to);

INLINE chunk_parser_mark chunk_parser_getmark(chunk_parser_ctx *ctx, const char *fpc);

/********************
 * Inline functions *
 ********************/

INLINE chunk_parser_mark chunk_parser_getmark(chunk_parser_ctx *ctx, const char *fpc) {
	chunk_parser_mark m;
	m.ci = ctx->curi;
	m.pos = ctx->start + fpc - ctx->buf;
	return m;
}

#define GETMARK(FPC) (chunk_parser_getmark(&ctx->chunk_ctx, FPC))

#endif

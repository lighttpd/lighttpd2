
#include <lighttpd/base.h>

void chunk_parser_init(liChunkParserCtx *ctx, liChunkQueue *cq) {
	ctx->cq = cq;
	chunk_parser_reset(ctx);
}

void chunk_parser_reset(liChunkParserCtx *ctx) {
	ctx->bytes_in = 0;
	ctx->curi.element = NULL;
	ctx->start = 0;
	ctx->length = 0;
	ctx->buf = NULL;
}

liHandlerResult chunk_parser_prepare(liChunkParserCtx *ctx) {
	if (NULL == ctx->curi.element) {
		ctx->curi = chunkqueue_iter(ctx->cq);
		if (NULL == ctx->curi.element) return LI_HANDLER_WAIT_FOR_EVENT;
	}
	return LI_HANDLER_GO_ON;
}

liHandlerResult chunk_parser_next(liVRequest *vr, liChunkParserCtx *ctx, char **p, char **pe) {
	off_t l;
	liHandlerResult res;

	if (NULL == ctx->curi.element) return LI_HANDLER_WAIT_FOR_EVENT;

	while (ctx->start >= (l = chunkiter_length(ctx->curi))) {
		liChunkIter i = ctx->curi;
		 /* Wait at the end of the last chunk if it gets extended */
		if (!chunkiter_next(&i)) return LI_HANDLER_WAIT_FOR_EVENT;
		ctx->curi = i;
		ctx->start -= l;
	}

	if (NULL == ctx->curi.element) return LI_HANDLER_WAIT_FOR_EVENT;

	if (LI_HANDLER_GO_ON != (res = chunkiter_read(vr, ctx->curi, ctx->start, l - ctx->start, &ctx->buf, &ctx->length))) {
		return res;
	}

	*p = ctx->buf;
	*pe = ctx->buf + ctx->length;
	return LI_HANDLER_GO_ON;
}

void chunk_parser_done(liChunkParserCtx *ctx, goffset len) {
	ctx->bytes_in += len;
	ctx->start += len;
}

gboolean chunk_extract_to(liVRequest *vr, liChunkParserMark from, liChunkParserMark to, GString *dest) {
	liChunkParserMark i;

	g_string_set_size(dest, 0);

	for ( i = from; i.ci.element != to.ci.element; chunkiter_next(&i.ci) ) {
		goffset len = chunkiter_length(i.ci);
		while (i.pos < len) {
			char *buf;
			off_t we_have;
			if (LI_HANDLER_GO_ON != chunkiter_read(vr, i.ci, i.pos, len - i.pos, &buf, &we_have)) goto error;
			g_string_append_len(dest, buf, we_have);
			i.pos += we_have;
		}
		i.pos = 0;
	}
	while (i.pos < to.pos) {
		char *buf;
		off_t we_have;
		if (LI_HANDLER_GO_ON != chunkiter_read(vr, i.ci, i.pos, to.pos - i.pos, &buf, &we_have)) goto error;
		g_string_append_len(dest, buf, we_have);
		i.pos += we_have;
	}

	return TRUE;

error:
	g_string_assign(dest, "");
	return FALSE;
}

GString* chunk_extract(liVRequest *vr, liChunkParserMark from, liChunkParserMark to) {
	GString *str = g_string_sized_new(0);
	if (chunk_extract_to(vr, from, to, str)) return str;
	g_string_free(str, TRUE);
	return NULL;
}

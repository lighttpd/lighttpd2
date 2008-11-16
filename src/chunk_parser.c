
#include <lighttpd/base.h>

void chunk_parser_init(chunk_parser_ctx *ctx, chunkqueue *cq) {
	ctx->cq = cq;
	chunk_parser_reset(ctx);
}

void chunk_parser_reset(chunk_parser_ctx *ctx) {
	ctx->bytes_in = 0;
	ctx->curi.element = NULL;
	ctx->start = 0;
	ctx->length = 0;
	ctx->buf = NULL;
}

handler_t chunk_parser_prepare(chunk_parser_ctx *ctx) {
	if (NULL == ctx->curi.element) {
		ctx->curi = chunkqueue_iter(ctx->cq);
		if (NULL == ctx->curi.element) return HANDLER_WAIT_FOR_EVENT;
	}
	return HANDLER_GO_ON;
}

handler_t chunk_parser_next(vrequest *vr, chunk_parser_ctx *ctx, char **p, char **pe) {
	off_t l;
	handler_t res;

	if (NULL == ctx->curi.element) return HANDLER_WAIT_FOR_EVENT;

	while (ctx->start >= (l = chunkiter_length(ctx->curi))) {
		chunkiter i = ctx->curi;
		 /* Wait at the end of the last chunk if it gets extended */
		if (!chunkiter_next(&i)) return HANDLER_WAIT_FOR_EVENT;
		ctx->curi = i;
		ctx->start -= l;
	}

	if (NULL == ctx->curi.element) return HANDLER_WAIT_FOR_EVENT;

	if (HANDLER_GO_ON != (res = chunkiter_read(vr, ctx->curi, ctx->start, l - ctx->start, &ctx->buf, &ctx->length))) {
		return res;
	}

	*p = ctx->buf;
	*pe = ctx->buf + ctx->length;
	return HANDLER_GO_ON;
}

void chunk_parser_done(chunk_parser_ctx *ctx, goffset len) {
	ctx->bytes_in += len;
	ctx->start += len;
}

gboolean chunk_extract_to(vrequest *vr, chunk_parser_mark from, chunk_parser_mark to, GString *dest) {
	g_string_set_size(dest, 0);

	chunk_parser_mark i;
	for ( i = from; i.ci.element != to.ci.element; chunkiter_next(&i.ci) ) {
		goffset len = chunkiter_length(i.ci);
		while (i.pos < len) {
			char *buf;
			off_t we_have;
			if (HANDLER_GO_ON != chunkiter_read(vr, i.ci, i.pos, len - i.pos, &buf, &we_have)) goto error;
			g_string_append_len(dest, buf, we_have);
			i.pos += we_have;
		}
		i.pos = 0;
	}
	while (i.pos < to.pos) {
		char *buf;
		off_t we_have;
		if (HANDLER_GO_ON != chunkiter_read(vr, i.ci, i.pos, to.pos - i.pos, &buf, &we_have)) goto error;
		g_string_append_len(dest, buf, we_have);
		i.pos += we_have;
	}

	return TRUE;

error:
	g_string_assign(dest, "");
	return FALSE;
}

GString* chunk_extract(vrequest *vr, chunk_parser_mark from, chunk_parser_mark to) {
	GString *str = g_string_sized_new(0);
	if (chunk_extract_to(vr, from, to, str)) return str;
	g_string_free(str, TRUE);
	return NULL;
}

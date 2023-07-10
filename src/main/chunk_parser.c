
#include <lighttpd/base.h>

void li_chunk_parser_init(liChunkParserCtx *ctx, liChunkQueue *cq) {
	ctx->cq = cq;
	li_chunk_parser_reset(ctx);
}

void li_chunk_parser_reset(liChunkParserCtx *ctx) {
	ctx->bytes_in = 0;
	ctx->curi.element = NULL;
	ctx->start = 0;
	ctx->length = 0;
	ctx->buf = NULL;
}

liHandlerResult li_chunk_parser_prepare(liChunkParserCtx *ctx) {
	if (NULL == ctx->curi.element) {
		ctx->curi = li_chunkqueue_iter(ctx->cq);
		if (NULL == ctx->curi.element) return LI_HANDLER_WAIT_FOR_EVENT;
	}
	return LI_HANDLER_GO_ON;
}

liHandlerResult li_chunk_parser_next(liChunkParserCtx *ctx, char **p, char **pe, GError **err) {
	off_t l;
	liHandlerResult res;

	g_return_val_if_fail (err == NULL || *err == NULL, LI_HANDLER_ERROR);

	if (NULL == ctx->curi.element) return LI_HANDLER_WAIT_FOR_EVENT;

	while (ctx->start >= (l = li_chunkiter_length(ctx->curi))) {
		liChunkIter i = ctx->curi;
		 /* Wait at the end of the last chunk if it gets extended */
		if (!li_chunkiter_next(&i)) return LI_HANDLER_WAIT_FOR_EVENT;
		ctx->curi = i;
		ctx->start -= l;
	}

	if (NULL == ctx->curi.element) return LI_HANDLER_WAIT_FOR_EVENT;

	if (LI_HANDLER_GO_ON != (res = li_chunkiter_read(ctx->curi, ctx->start, l - ctx->start, &ctx->buf, &ctx->length, err))) {
		return res;
	}

	*p = ctx->buf;
	*pe = ctx->buf + ctx->length;
	return LI_HANDLER_GO_ON;
}

void li_chunk_parser_done(liChunkParserCtx *ctx, goffset len) {
	ctx->bytes_in += len;
	ctx->start += len;
}

gboolean li_chunk_extract_to(liChunkParserMark from, liChunkParserMark to, GString *dest, GError **err) {
	liChunkParserMark i;

	g_return_val_if_fail (err == NULL || *err == NULL, FALSE);

	g_string_set_size(dest, to.abs_pos - from.abs_pos);
	li_g_string_clear(dest);

	for ( i = from; i.ci.element != to.ci.element; li_chunkiter_next(&i.ci) ) {
		goffset len = li_chunkiter_length(i.ci);
		while (i.pos < len) {
			char *buf;
			off_t we_have;
			if (LI_HANDLER_GO_ON != li_chunkiter_read(i.ci, i.pos, len - i.pos, &buf, &we_have, err)) goto error;
			if (dest->len + we_have < dest->allocated_len) {
				/* "fast" append */
				memcpy(dest->str + dest->len, buf, we_have);
				dest->len += we_have;
				dest->str[dest->len] = '\0';
			} else {
				li_g_string_append_len(dest, buf, we_have);
			}
			i.pos += we_have;
		}
		i.pos = 0;
	}
	while (i.pos < to.pos) {
		char *buf;
		off_t we_have;
		if (LI_HANDLER_GO_ON != li_chunkiter_read(i.ci, i.pos, to.pos - i.pos, &buf, &we_have, err)) goto error;
		if (dest->len + we_have < dest->allocated_len) {
			/* "fast" append */
			memcpy(dest->str + dest->len, buf, we_have);
			dest->len += we_have;
			dest->str[dest->len] = '\0';
		} else {
			li_g_string_append_len(dest, buf, we_have);
		}
		i.pos += we_have;
	}

	return TRUE;

error:
	li_g_string_clear(dest);
	return FALSE;
}

GString* li_chunk_extract(liChunkParserMark from, liChunkParserMark to, GError **err) {
	GString *str = g_string_sized_new(0);
	if (li_chunk_extract_to(from, to, str, err)) return str;
	g_string_free(str, TRUE);
	return NULL;
}

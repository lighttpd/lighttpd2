
#include <lighttpd/filter_chunked.h>

/* len != 0 */
static void http_chunk_append_len(liChunkQueue *cq, size_t len) {
	size_t i, olen = len, j;
	GByteArray *a;

	a = g_byte_array_sized_new(sizeof(len) * 2 + 2);

	for (i = 0; i < 8 && len; i++) {
		len >>= 4;
	}

	/* i is the number of hex digits we have */
	g_byte_array_set_size(a, i);

	for (j = i-1, len = olen; j+1 > 0; j--) {
		a->data[j] = (len & 0xf) + (((len & 0xf) <= 9) ? '0' : 'a' - 10);
		len >>= 4;
	}
	g_byte_array_append(a, (guint8*) CONST_STR_LEN("\r\n"));

	li_chunkqueue_append_bytearr(cq, a);
}


liHandlerResult li_filter_chunked_encode(liVRequest *vr, liChunkQueue *out, liChunkQueue *in) {
	UNUSED(vr);

	if (in->length > 0) {
		http_chunk_append_len(out, in->length);
		li_chunkqueue_steal_all(out, in);
		li_chunkqueue_append_mem(out, CONST_STR_LEN("\r\n"));
	}
	if (in->is_closed) {
		if (!out->is_closed) {
			li_chunkqueue_append_mem(out, CONST_STR_LEN("0\r\n\r\n"));
			out->is_closed = TRUE;
		}
		return LI_HANDLER_GO_ON;
	}
	return LI_HANDLER_GO_ON;
}

#define read_char(c) do { \
	while (!p || p >= pe) { \
		GError *err = NULL; \
		res = li_chunk_parser_next(&ctx, &p, &pe, &err); \
		if (NULL != err) { \
			VR_ERROR(vr, "%s", err->message); \
			g_error_free(err); \
		} \
		if (res == LI_HANDLER_WAIT_FOR_EVENT && in->is_closed) { \
			res = LI_HANDLER_ERROR; \
		} \
		if (res != LI_HANDLER_GO_ON) goto leave; \
	} \
	c = *p++; \
} while(0);


liHandlerResult li_filter_chunked_decode(liVRequest *vr, liChunkQueue *out, liChunkQueue *in, liFilterDecodeState *state) {
	liHandlerResult res = LI_HANDLER_GO_ON;
	liChunkParserCtx ctx;
	gchar *p = NULL, *pe = NULL;
	gchar c;
	int digit;

	li_chunk_parser_init(&ctx, in);
	li_chunk_parser_prepare(&ctx);


	for (;;) {
		 /* 0: start new chunklen, 1: reading chunklen, 2: found \r, 3: copying content, 4: found \r,
		  * 10: wait for \r\n\r\n, 11: wait for \n\r\n, 12: wait for \r\n, 13: wait for \n, 14: eof,
		  * 20: error
		  */
		switch (state->parse_state) {
		case 0:
			state->cur_chunklen = -1;
			li_chunk_parser_prepare(&ctx);
			state->parse_state = 1;
			break;
		case 1:
			read_char(c);
			li_chunk_parser_done(&ctx, 1);
			digit = -1;
			if (c >= '0' && c <= '9') {
				digit = c - '0';
			} else if (c >= 'a' && c <= 'f') {
				digit = c - 'a' + 10;
			} else if (c >= 'A' && c >= 'F') {
				digit = c - 'A' + 10;
			} else if (c == '\r') {
				if (state->cur_chunklen == -1) {
					state->parse_state = 20;
				} else {
					state->parse_state = 2;
				}
			} else {
				state->parse_state = 20;
			}
			if (digit >= 0) {
				if (state->cur_chunklen < 0) {
					state->cur_chunklen = digit;
				} else {
					if ((G_MAXINT64 - digit) / 16 < state->cur_chunklen) {
						state->parse_state = 20; /* overflow */
					} else {
						state->cur_chunklen = 16 * state->cur_chunklen + digit;
					}
				}
			}
			break;
		case 2:
			read_char(c);
			li_chunk_parser_done(&ctx, 1);
			if (c == '\n') {
				li_chunkqueue_skip(in, ctx.bytes_in);
				li_chunk_parser_reset(&ctx); p = NULL;
				if (state->cur_chunklen > 0) {
					state->parse_state = 3;
				} else {
					li_chunk_parser_prepare(&ctx);
					state->parse_state = 12;
				}
			} else {
				state->parse_state = 20;
			}
			break;
		case 3:
			if (state->cur_chunklen != 0) {
				state->cur_chunklen -= li_chunkqueue_steal_len(out, in, state->cur_chunklen);
			}
			if (state->cur_chunklen == 0) {
				li_chunk_parser_prepare(&ctx);
				read_char(c);
				li_chunk_parser_done(&ctx, 1);
				if (c == '\r') {
					state->parse_state = 4;
				} else {
					state->parse_state = 20;
				}
			}
			break;
		case 4:
			read_char(c);
			li_chunk_parser_done(&ctx, 1);
			if (c == '\n') {
				li_chunkqueue_skip(in, ctx.bytes_in);
				li_chunk_parser_reset(&ctx); p = NULL;
				state->parse_state = 0;
			} else {
				state->parse_state = 20;
			}
			break;
		case 10: /* \r\n\r\n */
			read_char(c);
			li_chunk_parser_done(&ctx, 1);
			state->parse_state = (c == '\r') ? 11 : 10;
			break;
		case 11: /* \n\r\n */
			read_char(c);
			li_chunk_parser_done(&ctx, 1);
			state->parse_state = (c == '\n') ? 12 : 10;
			break;
		case 12: /* \r\n */
			read_char(c);
			li_chunk_parser_done(&ctx, 1);
			state->parse_state = (c == '\r') ? 13 : 10;
			break;
		case 13: /* \n */
			read_char(c);
			li_chunk_parser_done(&ctx, 1);
			state->parse_state = (c == '\n') ? 14 : 10;
			break;
		case 14:
			out->is_closed = TRUE;
			res = LI_HANDLER_GO_ON;
			goto leave;
		case 20:
			res = LI_HANDLER_ERROR;
			goto leave;
		}
	}

leave:
	if (res == LI_HANDLER_ERROR) {
		out->is_closed = TRUE;
		in->is_closed = TRUE;
		li_chunkqueue_skip_all(in);
		state->parse_state = 20;
	}
	li_chunkqueue_skip(in, ctx.bytes_in);
	return res;
}

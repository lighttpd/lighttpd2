
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


liHandlerResult li_filter_chunked_encode(liConnection *con, liChunkQueue *out, liChunkQueue *in) {
	UNUSED(con);

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

liHandlerResult li_filter_chunked_decode(liConnection *con, liChunkQueue *out, liChunkQueue *in) {
	UNUSED(con);
	UNUSED(out);
	UNUSED(in);
	return LI_HANDLER_ERROR;
}

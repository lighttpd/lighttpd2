
#include <lighttpd/filter_chunked.h>

/* len != 0 */
static void http_chunk_append_len(chunkqueue *cq, size_t len) {
	size_t i, olen = len, j;
	GString *s;

	s = g_string_sized_new(sizeof(len) * 2 + 2);

	for (i = 0; i < 8 && len; i++) {
		len >>= 4;
	}

	/* i is the number of hex digits we have */
	g_string_set_size(s, i);

	for (j = i-1, len = olen; j+1 > 0; j--) {
		s->str[j] = (len & 0xf) + (((len & 0xf) <= 9) ? '0' : 'a' - 10);
		len >>= 4;
	}
	g_string_append_len(s, CONST_STR_LEN("\r\n"));

	chunkqueue_append_string(cq, s);
}


handler_t filter_chunked_encode(connection *con, chunkqueue *out, chunkqueue *in) {
	UNUSED(con);

	if (in->length > 0) {
		http_chunk_append_len(out, in->length);
		chunkqueue_steal_all(out, in);
		chunkqueue_append_mem(out, CONST_STR_LEN("\r\n"));
	}
	if (in->is_closed) {
		if (!out->is_closed) {
			chunkqueue_append_mem(out, CONST_STR_LEN("0\r\n\r\n"));
			out->is_closed = TRUE;
		}
		return HANDLER_GO_ON;
	}
	return HANDLER_GO_ON;
}

handler_t filter_chunked_decode(connection *con, chunkqueue *out, chunkqueue *in) {
	UNUSED(con);
	UNUSED(out);
	UNUSED(in);
	return HANDLER_ERROR;
}

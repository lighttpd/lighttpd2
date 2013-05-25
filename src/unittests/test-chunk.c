
#include <lighttpd/base.h>

#define perror(msg) g_error("(%s:%i) %s failed: %s", __FILE__, __LINE__, msg, g_strerror(errno))

#if 0
static liChunkQueue* cq_from_str(const gchar *s, size_t len) {
	liChunkQueue *cq = li_chunkqueue_new();
	li_chunkqueue_append_mem(cq, s, len);
	return cq;
}
#endif

static void cq_load_str(liChunkQueue *cq, const gchar *s, size_t len) {
	li_chunkqueue_reset(cq);
	li_chunkqueue_append_mem(cq, s, len);
}

static void cq_assert_eq(liChunkQueue *cq, const gchar *s, size_t len) {
	GString *buf = g_string_sized_new(cq->length);
	g_assert(li_chunkqueue_extract_to(cq, cq->length, buf, NULL));
	g_assert(0 == memcmp(s, buf->str, len));
	g_string_free(buf, TRUE);
}


static void test_filter_chunked_decode(void) {
	liChunkQueue *cq = li_chunkqueue_new(), *cq2 = li_chunkqueue_new();
	liFilterChunkedDecodeState decode_state;

	cq_load_str(cq, CONST_STR_LEN(
		"14\r\n"
		"01234567890123456789" "\r\n"
		"0\r\nrandom foo: xx\r\n\r\n"
		"xxx" /* next message */
	));
	cq->is_closed = TRUE;
	memset(&decode_state, 0, sizeof(decode_state));
	li_chunkqueue_reset(cq2);
	g_assert(li_filter_chunked_decode(NULL, cq2, cq, &decode_state));
	cq_assert_eq(cq2, CONST_STR_LEN(
		"01234567890123456789"
	));
	g_assert(cq2->is_closed);
	cq_assert_eq(cq, CONST_STR_LEN(
		"xxx"
	));

	li_chunkqueue_free(cq);
	li_chunkqueue_free(cq2);
}

int main(int argc, char **argv) {
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/chunk/filter_chunked_decode", test_filter_chunked_decode);

	return g_test_run();
}

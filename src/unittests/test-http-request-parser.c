
#include <lighttpd/base.h>
#include <lighttpd/http_request_parser.h>

static void test_crlf_newlines(void) {
	liRequest req;
	liHttpRequestCtx http_req_ctx;
	liChunkQueue* cq = li_chunkqueue_new();
	liHandlerResult res;

	li_chunkqueue_append_mem(cq, CONST_STR_LEN(
		"GET / HTTP/1.0\r\n"
		"Host: www.example.com\r\n"
		"\r\n"
		"\ntrash"));
	li_request_init(&req);
	li_http_request_parser_init(&http_req_ctx, &req, cq);

	res = li_http_request_parse(NULL, &http_req_ctx);
	if (LI_HANDLER_GO_ON != res) g_error("li_http_request_parse didn't finish parsing or failed: %i", res);

	g_assert_true(6 == cq->length);
	g_assert_true(li_http_header_is(req.headers, CONST_STR_LEN("host"), CONST_STR_LEN("www.example.com")));

	li_chunkqueue_free(cq);
	li_http_request_parser_clear(&http_req_ctx);
	li_request_clear(&req);
}

static void test_lf_newlines(void) {
	liRequest req;
	liHttpRequestCtx http_req_ctx;
	liChunkQueue* cq = li_chunkqueue_new();
	liHandlerResult res;

	li_chunkqueue_append_mem(cq, CONST_STR_LEN(
		"GET / HTTP/1.0\n"
		"Host: www.example.com\n"
		"\n"
		"\rtrash"));
	li_request_init(&req);
	li_http_request_parser_init(&http_req_ctx, &req, cq);

	res = li_http_request_parse(NULL, &http_req_ctx);
	if (LI_HANDLER_GO_ON != res) g_error("li_http_request_parse didn't finish parsing or failed: %i", res);

	g_assert_true(6 == cq->length);
	g_assert_true(li_http_header_is(req.headers, CONST_STR_LEN("host"), CONST_STR_LEN("www.example.com")));

	li_chunkqueue_free(cq);
	li_http_request_parser_clear(&http_req_ctx);
	li_request_clear(&req);
}

int main(int argc, char **argv) {
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/http-request-parser/crlf_newlines", test_crlf_newlines);
	g_test_add_func("/http-request-parser/lf_newlines", test_lf_newlines);

	return g_test_run();
}

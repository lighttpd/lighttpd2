
#include "base.h"

void request_init(request *req, chunkqueue *in) {
	req->http_method = HTTP_METHOD_UNSET;
	req->http_method_str = g_string_sized_new(0);
	req->http_version = HTTP_VERSION_UNSET;

	req->uri.uri = g_string_sized_new(0);
	req->uri.orig_uri = g_string_sized_new(0);
	req->uri.scheme = g_string_sized_new(0);
	req->uri.path = g_string_sized_new(0);
	req->uri.query = g_string_sized_new(0);

	req->headers = http_headers_new();

	req->host = g_string_sized_new(0);
	req->content_length = -1;

	http_request_parser_init(&req->parser_ctx, req, in);
}

void request_reset(request *req) {
	req->http_method = HTTP_METHOD_UNSET;
	g_string_truncate(req->http_method_str, 0);
	req->http_version = HTTP_VERSION_UNSET;

	g_string_truncate(req->uri.uri, 0);
	g_string_truncate(req->uri.orig_uri, 0);
	g_string_truncate(req->uri.scheme, 0);
	g_string_truncate(req->uri.path, 0);
	g_string_truncate(req->uri.query, 0);

	http_headers_reset(req->headers);

	g_string_truncate(req->host, 0);
	req->content_length = -1;

	http_request_parser_reset(&req->parser_ctx);
}

void request_clear(request *req) {
	req->http_method = HTTP_METHOD_UNSET;
	g_string_free(req->http_method_str, TRUE);
	req->http_version = HTTP_VERSION_UNSET;

	g_string_free(req->uri.uri, TRUE);
	g_string_free(req->uri.orig_uri, TRUE);
	g_string_free(req->uri.scheme, TRUE);
	g_string_free(req->uri.path, TRUE);
	g_string_free(req->uri.query, TRUE);

	http_headers_free(req->headers);

	g_string_free(req->host, TRUE);
	req->content_length = -1;

	http_request_parser_clear(&req->parser_ctx);
}

void request_validate_header(server *srv, connection *con) {
	switch(con->request.http_method) {
	case HTTP_METHOD_GET:
	case HTTP_METHOD_HEAD:
		/* content-length is forbidden for those */
		if (con->request.content_length > 0) {
			/* content-length is missing */
			CON_ERROR(srv, con, "%s", "GET/HEAD with content-length -> 400");

			con->keep_alive = FALSE;
			con->response.http_status = 400;
			connection_handle_direct(srv, con);
			return;
		}
		con->request.content_length = 0;
		break;
	case HTTP_METHOD_POST:
		/* content-length is required for them */
		if (con->request.content_length == -1) {
			/* content-length is missing */
			CON_ERROR(srv, con, "%s", "POST-request, but content-length missing -> 411");

			con->keep_alive = FALSE;
			con->response.http_status = 411;
			connection_handle_direct(srv, con);
			return;
		}
		break;
	default:
		/* the may have a content-length */
		break;
	}
}


#include "request.h"

request* request_new() {
	request *req = g_slice_new0(request);

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

	return req;
}

void request_free(request *req) {
	/* TODO */
	g_slice_free(request, req);
}

#ifndef _LIGHTTPD_HTTP_REQUEST_PARSER_H_
#define _LIGHTTPD_HTTP_REQUEST_PARSER_H_

#ifndef _LIGHTTPD_BASE_H_
#error Please include "base.h" instead of this file
#endif

struct http_request_ctx;
typedef struct http_request_ctx http_request_ctx;

struct http_request_ctx {
	chunk_parser_ctx chunk_ctx;
	struct request *request;

	chunk_parser_mark mark;
	GString *h_key, *h_value;
};

LI_API void http_request_parser_init(http_request_ctx* ctx, request *req, chunkqueue *cq);
LI_API void http_request_parser_reset(http_request_ctx* ctx);
LI_API void http_request_parser_clear(http_request_ctx *ctx);

LI_API handler_t http_request_parse(vrequest *vr, http_request_ctx *ctx);


#endif

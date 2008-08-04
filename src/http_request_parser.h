#ifndef _LIGHTTPD_HTTP_REQUEST_PARSER_H_
#define _LIGHTTPD_HTTP_REQUEST_PARSER_H_

struct http_request_ctx;
typedef struct http_request_ctx http_request_ctx;

#include "chunk_parser.h"
#include "request.h"

struct http_request_ctx {
	chunk_parser_ctx chunk_ctx;
	request *request;

	chunk_parser_mark mark;
	GString *h_key, *h_value;
};

LI_API http_request_ctx* http_request_parser_new(request *req, chunkqueue *cq);
LI_API void http_request_parser_free(http_request_ctx *ctx);

LI_API handler_t http_request_parse(server *srv, connection *con, http_request_ctx *ctx);


#endif

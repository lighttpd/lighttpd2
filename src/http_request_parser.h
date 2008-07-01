#ifndef _LIGHTTPD_HTTP_REQUEST_PARSER_H_
#define _LIGHTTPD_HTTP_REQUEST_PARSER_H_

struct http_request_ctx;
typedef struct http_request_ctx http_request_ctx;

#include "chunk_parser.h"
#include "request.h"

struct http_request_ctx {
	chunk_parser_ctx chunk_ctx;

	chunk_parser_mark mark;

	request *request;
};

#endif

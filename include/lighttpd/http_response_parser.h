#ifndef _LIGHTTPD_HTTP_RESPONSE_PARSER_H_
#define _LIGHTTPD_HTTP_RESPONSE_PARSER_H_

#ifndef _LIGHTTPD_BASE_H_
#error Please include <lighttpd/base.h> instead of this file
#endif

struct http_response_ctx;
typedef struct http_response_ctx http_response_ctx;

struct http_response_ctx {
	chunk_parser_ctx chunk_ctx;
	struct response *response;

	gboolean accept_cgi, accept_nph;

	chunk_parser_mark mark;
	GString *h_key, *h_value;
};

LI_API void http_response_parser_init(http_response_ctx* ctx, response *req, chunkqueue *cq, gboolean accept_cgi, gboolean accept_nph);
LI_API void http_response_parser_reset(http_response_ctx* ctx);
LI_API void http_response_parser_clear(http_response_ctx *ctx);

LI_API handler_t http_response_parse(vrequest *vr, http_response_ctx *ctx);


#endif

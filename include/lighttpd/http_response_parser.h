#ifndef _LIGHTTPD_HTTP_RESPONSE_PARSER_H_
#define _LIGHTTPD_HTTP_RESPONSE_PARSER_H_

#ifndef _LIGHTTPD_BASE_H_
#error Please include <lighttpd/base.h> instead of this file
#endif

struct liHttpResponseCtx {
	liChunkParserCtx chunk_ctx;
	liResponse *response;

	gboolean accept_cgi, accept_nph;

	liChunkParserMark mark;
	GString *h_key, *h_value;
};

LI_API void http_response_parser_init(liHttpResponseCtx* ctx, liResponse *req, liChunkQueue *cq, gboolean accept_cgi, gboolean accept_nph);
LI_API void http_response_parser_reset(liHttpResponseCtx* ctx);
LI_API void http_response_parser_clear(liHttpResponseCtx *ctx);

LI_API liHandlerResult http_response_parse(liVRequest *vr, liHttpResponseCtx *ctx);


#endif

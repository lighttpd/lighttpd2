#ifndef _LIGHTTPD_HTTP_REQUEST_PARSER_H_
#define _LIGHTTPD_HTTP_REQUEST_PARSER_H_

#ifndef _LIGHTTPD_BASE_H_
#error Please include <lighttpd/base.h> instead of this file
#endif

struct liHttpRequestCtx {
	liChunkParserCtx chunk_ctx;
	liRequest *request;

	liChunkParserMark mark;
	GString *h_key, *h_value;
};

LI_API void http_request_parser_init(liHttpRequestCtx* ctx, liRequest *req, liChunkQueue *cq);
LI_API void http_request_parser_reset(liHttpRequestCtx* ctx);
LI_API void http_request_parser_clear(liHttpRequestCtx *ctx);

LI_API liHandlerResult http_request_parse(liVRequest *vr, liHttpRequestCtx *ctx);


#endif

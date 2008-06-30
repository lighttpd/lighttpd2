
#include "http_request_parser.h"

static chunk_parser_mark getmark(http_request_ctx *ctx, const char *fpc) {
	chunk_parser_mark m;
	m.ci = ctx->curi;
	m.pos = ctx->start + fpc - ctx->buf;
	return m;
}

/** Machine **/

%%{

	machine http_request_parser;

	CRLF = "\r\n";

	action mark { ctx->mark = getmark(ctx, fpc); }

	main := CRLF ;
}%%

%% write data;

void http_request_parse(server *srv, connection *con, http_request_ctx *ctx) {
	int cs = ctx->cs;
	while (cs != http_request_parser_error && cs != http_request_parser_first_final) {
		char *p, *pe;
		off_t l;

		l = chunkiter_length(ctx->curi);
		if (ctx->start >= l) {
			chunkiter_next(&ctx->curi);
			continue;
		}

		if (HANDLER_GO_ON != chunkiter_read(srv, con, ctx->curi, ctx->start, l - ctx->start, &ctx->buf, &ctx->length)) {
			return;
		}

		p = ctx->buf;
		pe = ctx->buf + ctx->length;

		%% write exec;

		ctx->start += pe - p;
		ctx->bytes_in += pe - p;
		if (ctx->start >= l) {
			chunkiter_next(&ctx->curi);
			ctx->start = 0;
		}
	}
	ctx->cs = cs;
}

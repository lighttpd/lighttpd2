
#include "http_request_parser.h"

/** Machine **/

#define _getString(M, FPC) (chunk_extract(srv, con, ctx->M, GETMARK(FPC)))
#define getString(FPC) _getString(mark, FPC)

%%{

	machine http_request_parser;
	variable cs ctx->chunk_ctx.cs;

	action mark { ctx->mark = GETMARK(fpc); }
	action done { fbreak; }

	action method { ctx->request->http_method_str = getString(fpc); }
	action uri { ctx->request->uri.uri = getString(fpc); }

# RFC 2616
	OCTET = any;
	CHAR = ascii;
	UPALPHA = upper;
	LOALPHA = lower;
	ALPHA = alpha;
	DIGIT = digit;
	CTL = ( 0 .. 31 | 127 );
	CR = '\r';
	LF = '\n';
	SP = ' ';
	HT = '\t';
	DQUOTE = '"';

	CRLF = CR LF;
	LWS = CRLF? (SP | HT)+; # linear white space
	TEXT = (OCTET - CTL) | LWS;
	HEX = [a-fA-F0-9];

	Separators = [()<>@,;:\\\"/\[\]?={}] | SP | HT;
	Token = (OCTET - Separators - CTL)+;

	# original definition
	# Comment = "(" ( CText | Quoted_Pair | Comment )* ")";
	# CText   = TEXT - [()];

	Quoted_Pair    = "\\" CHAR;
	Comment        = ( TEXT | Quoted_Pair )*;
	QDText         = TEXT - DQUOTE;
	Quoted_String   = DQUOTE ( QDText | Quoted_Pair )* DQUOTE;

	HTTP_Version = "HTTP" "/" DIGIT+ "." DIGIT+;
	#HTTP_URL = "http:" "//" Host ( ":" Port )? ( abs_path ( "?" query )? )?;

# RFC 2396

	Mark = [\-_!~*\'()];
	Unreserved = alnum | Mark;
	Escaped = "%" HEX HEX;

	PChar = Unreserved | Escaped | [:@&=+$,];
	Segment = PChar* ( ";" PChar* )*;
	Path_Segments = Segment ("/" Segment)*;
	Abs_Path = "/" Path_Segments;

	Method = Token >mark %method;
	Request_URI = ("*" | ( any - CTL - SP )) >mark %uri;
	Request_Line = Method " " Request_URI " " HTTP_Version CRLF;

	Field_Content = ( TEXT+ | ( Token | Separators | Quoted_String )+ );
	Field_Value = " "* (Field_Content+ ( Field_Content | LWS )*)? >mark;
	Message_Header = Token ":" Field_Value?;

	main := (CRLF)* Request_Line (Message_Header CRLF)* CRLF @ done;
}%%

%% write data;

static int http_request_parser_has_error(http_request_ctx *ctx) {
	return ctx->chunk_ctx.cs == http_request_parser_error;
}

static int http_request_parser_is_finished(http_request_ctx *ctx) {
	return ctx->chunk_ctx.cs >= http_request_parser_first_final;
}

void http_request_parser_init(http_request_ctx *ctx, request *req, chunkqueue *cq) {
	%% write init;
	chunk_parser_init(&ctx->chunk_ctx, cq);
	ctx->request = req;
}

handler_t http_request_parse(server *srv, connection *con, http_request_ctx *ctx) {
	handler_t res;

	if (HANDLER_GO_ON != (res = chunk_parser_prepare(&ctx->chunk_ctx))) return res;

	while (!http_request_parser_has_error(ctx) && !http_request_parser_is_finished(ctx)) {
		char *p, *pe;

		if (HANDLER_GO_ON != (res = chunk_parser_next(srv, con, &ctx->chunk_ctx, &p, &pe))) return res;

		%% write exec;

		chunk_parser_done(&ctx->chunk_ctx, pe - p);
	}

	if (http_request_parser_has_error(ctx)) return HANDLER_ERROR;
	if (http_request_parser_is_finished(ctx)) return HANDLER_GO_ON;
	return HANDLER_ERROR;
}

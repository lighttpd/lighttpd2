#ifndef _LIGHTTPD_HTTP_REQUEST_PARSER_H_
#define _LIGHTTPD_HTTP_REQUEST_PARSER_H_

struct http_request_ctx;
typedef struct http_request_ctx http_request_ctx;

#include "chunks.h"

struct http_request_ctx {
	chunkqueue *cq;

	goffset bytes_in;

	/* current position
	 * buf is curi[start..start+length)
	 */
	chunkiter curi;
	off_t start, length;
	char *buf;

	int cs;

	chunk_parser_mark mark;
};

#endif

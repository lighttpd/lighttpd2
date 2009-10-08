#ifndef _LIGHTTPD_HTTP_RANGE_PARSER_H_
#define _LIGHTTPD_HTTP_RANGE_PARSER_H_

#include <lighttpd/base.h>

typedef struct {
	/* public */
	gboolean last_range;
	goffset range_start, range_length, range_end; /* length = end - start + 1; */

	/* private */
	GString *data;
	goffset limit; /* "file size" */
	gboolean found_valid_range;

	int cs;
	gchar *data_pos;
} liParseHttpRangeState;

typedef enum {
	LI_PARSE_HTTP_RANGE_OK,
	LI_PARSE_HTTP_RANGE_DONE,
	LI_PARSE_HTTP_RANGE_INVALID,
	LI_PARSE_HTTP_RANGE_NOT_SATISFIABLE
} liParseHttpRangeResult;

LI_API void li_parse_http_range_init(liParseHttpRangeState* s, const GString *range_str, goffset limit);
LI_API liParseHttpRangeResult li_parse_http_range_next(liParseHttpRangeState* s);
LI_API void li_parse_http_range_clear(liParseHttpRangeState* s);

#endif

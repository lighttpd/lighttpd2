
#include <lighttpd/base.h>
#include <lighttpd/http_range_parser.h>

#include <stdlib.h>

%%{
	machine http_range_parser;
	variable cs s->cs;
	variable p s->data_pos;

	SP = ' ';
	HT = '\t';

	ws = SP | HT;

	action int_start {
		tmp = 0;
	}
	action int_step {
		int d = fc - '0';
		if (tmp > (G_MAXOFFSET-10)/10) {
			s->cs = http_range_parser_error;
			return LI_PARSE_HTTP_RANGE_INVALID;
		}
		tmp = 10*tmp + d;
	}

	int = (digit digit**) >int_start $int_step ;

	action first_byte {
		s->range_start = tmp;
	}
	action last_byte {
		s->range_end = tmp;
		found = TRUE;
	}
	action last_byte_empty {
		s->range_end = s->limit - 1;
		found = TRUE;
	}
	action suffix_range {
		s->range_end = s->limit - 1;
		s->range_start = s->limit - tmp;
		found = TRUE;
	}
	action range_complete {
		fbreak;
	}

	range = (int %first_byte ws* "-" ws** (int %last_byte | "" %last_byte_empty) | "-" ws* int %suffix_range) ;

	main := ws* "bytes" ws* "=" (ws | ",")* range ( ws* "," >range_complete (ws | ",")* range)** (ws | ",")*;

	write data;
}%%

liParseHttpRangeResult li_parse_http_range_next(liParseHttpRangeState* s) {
	const char *pe, *eof;
	goffset tmp = 0;

	if (s->cs == http_range_parser_error) {
		return LI_PARSE_HTTP_RANGE_INVALID;
	}

	eof = pe = s->data->str + s->data->len;

	for ( ;; ) {
		gboolean found = FALSE;

		if (s->data_pos >= eof) {
			return s->found_valid_range ? LI_PARSE_HTTP_RANGE_DONE : LI_PARSE_HTTP_RANGE_NOT_SATISFIABLE;
		}

		%% write exec;

		if (s->cs == http_range_parser_error) {
			return LI_PARSE_HTTP_RANGE_INVALID;
		}

		if (s->data_pos >= eof) {
			s->last_range = TRUE;
		}

		if (!found) {
			return s->found_valid_range ? LI_PARSE_HTTP_RANGE_DONE : LI_PARSE_HTTP_RANGE_NOT_SATISFIABLE;
		}

		if (s->range_end >= s->limit) {
			s->range_end = s->limit - 1;
		}
		if (s->range_start < 0) {
			s->range_start = 0;
		}

		if (s->range_start <= s->range_end) {
			s->found_valid_range = TRUE;
			s->range_length = s->range_end - s->range_start + 1;
			return LI_PARSE_HTTP_RANGE_OK;
		}
	}
}

void li_parse_http_range_init(liParseHttpRangeState* s, const GString *range_str, goffset limit) {
	s->data = g_string_new_len(GSTR_LEN(range_str));
	s->data_pos = s->data->str;
	s->limit = limit;
	s->last_range = FALSE;
	s->found_valid_range = FALSE;

	%% write init;
}

void li_parse_http_range_clear(liParseHttpRangeState* s) {
	if (s->data) {
		g_string_free(s->data, TRUE);
		s->data = NULL;
	}
}

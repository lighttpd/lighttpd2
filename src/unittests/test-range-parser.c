
#include <lighttpd/base.h>
#include <lighttpd/http_range_parser.h>

#define LIMIT_EXAMPLE (1409328ul)

typedef struct {
	liParseHttpRangeResult res;
	goffset start, end;
} rangeentry;

static const gchar* rangeresult_str(liParseHttpRangeResult res) {
	switch (res) {
	case LI_PARSE_HTTP_RANGE_OK: return "Range-OK";
	case LI_PARSE_HTTP_RANGE_DONE: return "Range-Done";
	case LI_PARSE_HTTP_RANGE_INVALID: return "Range-Invalid";
	case LI_PARSE_HTTP_RANGE_NOT_SATISFIABLE: return "Range-NotSatisfiable";
	default: return "RangeResult-Unknown";
	}
}

#define range_error(fmt, ...) g_error("li_parse_http_range_next error in round %i for '%s' (remaining '%s'): " fmt, round, range->str, s.data_pos, __VA_ARGS__)

static void test_range(GString *range, goffset limit, const rangeentry *results) {
	liParseHttpRangeState s;
	liParseHttpRangeResult res = LI_PARSE_HTTP_RANGE_OK;
	guint round = 1;

	li_parse_http_range_init(&s, range, limit);

	for ( ; res == LI_PARSE_HTTP_RANGE_OK; results++, round++ ) {
#if 0
		g_print("Round %i, parse '%s', state: %i\n", round, s.data_pos, s.cs);
#endif
		res = li_parse_http_range_next(&s);
		if (res != results->res) {
			range_error("unexpected parse result '%s' (expected '%s')", rangeresult_str(res), rangeresult_str(results->res));
		}
		if (res != LI_PARSE_HTTP_RANGE_OK) break;

		if (s.range_length != s.range_end - s.range_start + 1) {
			range_error("unexpected range length %"LI_GOFFSET_FORMAT" (expected %"LI_GOFFSET_FORMAT")", s.range_length, s.range_end - s.range_start + 1);
		} else if (s.range_start != results->start) {
			range_error("unexpected range start %"LI_GOFFSET_FORMAT" (expected %"LI_GOFFSET_FORMAT")", s.range_start, results->start);
		} else if (s.range_end != results->end) {
			range_error("unexpected range end %"LI_GOFFSET_FORMAT" (expected %"LI_GOFFSET_FORMAT")", s.range_end, results->end);
		}
	}

	li_parse_http_range_clear(&s);
}


static void test_range_parser_ex1(void) {
	static const rangeentry results[] = {
		{ LI_PARSE_HTTP_RANGE_OK, LIMIT_EXAMPLE - 500, LIMIT_EXAMPLE - 1 },
		{ LI_PARSE_HTTP_RANGE_OK, 10, LIMIT_EXAMPLE - 1 },
		{ LI_PARSE_HTTP_RANGE_OK, 5, 9 },
		{ LI_PARSE_HTTP_RANGE_DONE, -1, -1 }
	};
	GString range = li_const_gstring(CONST_STR_LEN("bytes=-500,,10-,5-9,"));

	test_range(&range, LIMIT_EXAMPLE, results);
}

static void test_range_parser_ex2(void) {
	static const rangeentry results[] = {
		{ LI_PARSE_HTTP_RANGE_OK, LIMIT_EXAMPLE - 500, LIMIT_EXAMPLE - 1 },
		{ LI_PARSE_HTTP_RANGE_OK, 10, LIMIT_EXAMPLE - 1 },
		{ LI_PARSE_HTTP_RANGE_OK, 5, 9 },
		{ LI_PARSE_HTTP_RANGE_DONE, -1, -1 }
	};
	GString range = li_const_gstring(CONST_STR_LEN("bytes =  , -500, ,, ,10- ,5-9  ,,"));

	test_range(&range, LIMIT_EXAMPLE, results);
}

static void test_range_parser_ex3(void) {
	static const rangeentry results[] = {
		{ LI_PARSE_HTTP_RANGE_NOT_SATISFIABLE, -1, -1 }
	};
	GString range = li_const_gstring(CONST_STR_LEN("bytes=0"));

	test_range(&range, LIMIT_EXAMPLE, results);
}

static void test_range_parser_ex4(void) {
	static const rangeentry results[] = {
		{ LI_PARSE_HTTP_RANGE_OK, 0, LIMIT_EXAMPLE - 1 },
		{ LI_PARSE_HTTP_RANGE_DONE, -1, -1 }
	};
	GString range = li_const_gstring(CONST_STR_LEN("bytes=0-"));

	test_range(&range, LIMIT_EXAMPLE, results);
}

int main(int argc, char **argv) {
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/range-parser/range_example_1", test_range_parser_ex1);
	g_test_add_func("/range-parser/range_example_2", test_range_parser_ex2);
	g_test_add_func("/range-parser/range_example_3", test_range_parser_ex3);
	g_test_add_func("/range-parser/range_example_4", test_range_parser_ex4);

	return g_test_run();
}

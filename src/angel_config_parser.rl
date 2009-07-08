#include <lighttpd/angel_base.h>
#include <lighttpd/angel_config_parser.h>

typedef struct {
	GArray *g_stack;
	int *stack;
	int cs, top;

	/* readingtoken signals that the parser is reading a token; in case
	 * the data cames in chunks, the parse function has to append the
	 * data to the token and handle tokenstart in the next chunk
	 */
	GString *token;
	gboolean readingtoken;

	char hexchar;
	gint64 number, number2;
	gboolean negate; /* sign for parsing numbers */

	/* item */
	GString *itemname;
	liValue *itemvalue;

	/* every time a collection gets parsed, the collection is pushed on the stack.
	 * in some other cases a LI_VALUE_STRING gets pushed (the key in hashes for example)
	 * while a second value gets parsed.
	 */
	GPtrArray *valuestack;
	liValue *curvalue;

	/* temporary buffer to format a character for logging */
	gchar buf[8];
} pcontext;

typedef struct {
	const gchar *filename;
	int line, column;
} filecontext;

GQuark angel_config_parser_error_quark() {
	return g_quark_from_static_string("angel-config-parser-error-quark");
}

static gchar *format_char(pcontext *ctx, gchar c) {
	if (g_ascii_isprint(c)) {
		ctx->buf[0] = c;
		ctx->buf[1] = '\0';
	} else switch (c) {
	case '\n': ctx->buf[0] = '\\'; ctx->buf[1] = 'n'; ctx->buf[2] = '\0'; break;
	case '\r': ctx->buf[0] = '\\'; ctx->buf[1] = 'r'; ctx->buf[2] = '\0'; break;
	case '\t': ctx->buf[0] = '\\'; ctx->buf[1] = 't'; ctx->buf[2] = '\0'; break;
	default: g_snprintf(ctx->buf, 8, "\\x%02X", (unsigned int) (unsigned char) c); break;
	}
	return ctx->buf;
}

#define PARSE_ERROR_FMT(fmt, ...) do { \
	g_set_error(err, \
		LI_ANGEL_CONFIG_PARSER_ERROR, \
		LI_ANGEL_CONFIG_PARSER_ERROR_PARSE, \
		"Parsing failed in '%s:%i,%i': " fmt, \
		fctx->filename, fctx->line, fctx->column, \
		__VA_ARGS__); \
} while (0)

#define PARSE_ERROR(msg) PARSE_ERROR_FMT("%s", msg)

#define UPDATE_COLUMN() do { \
	fctx->column += p - linestart; \
	linestart = p; \
} while (0)

%%{
	machine angel_config_parser;

	access ctx->;

	prepush { update_stack(ctx); }
	postpop { update_stack(ctx); }

	action starttoken {
		g_string_truncate(ctx->token, 0);
		tokenstart = fpc;
		ctx->readingtoken = TRUE;
	}
	action endtoken {
		g_string_append_len(ctx->token, tokenstart, fpc - tokenstart);
		tokenstart = NULL;
		ctx->readingtoken = FALSE;
	}

	action newline {
		fctx->line++;
		fctx->column = 1;
		linestart = fpc+1;
	}

	action startitem {
		ctx->itemname = ctx->token;
		ctx->token = g_string_sized_new(0);
		ctx->itemvalue = value_new_hash();
	}

	action enditem {
		plugins_handle_item(srv, ctx->itemname, ctx->itemvalue);
		g_string_free(ctx->itemname, TRUE);
		ctx->itemname = NULL;
		value_free(ctx->itemvalue);
		ctx->itemvalue = NULL;
	}

	action error_unknown {
		UPDATE_COLUMN();
		PARSE_ERROR("internal parse error");
		ctx->cs = angel_config_parser_error; fbreak;
	}

	action error_unexpected_char {
		UPDATE_COLUMN();
		PARSE_ERROR_FMT("unexpected character '%s'", format_char(ctx, fc));
		ctx->cs = angel_config_parser_error; fbreak;
	}

	action error_expected_id {
		UPDATE_COLUMN();
		PARSE_ERROR_FMT("unexpected character '%s', expected identifier", format_char(ctx, fc));
		ctx->cs = angel_config_parser_error; fbreak;
	}

	action error_expected_block {
		UPDATE_COLUMN();
		PARSE_ERROR_FMT("unexpected character '%s', expected block ('{...}' or ';') for item '%s'", format_char(ctx, fc), ctx->itemname->str);
		ctx->cs = angel_config_parser_error; fbreak;
	}

	action error_expected_xdigit {
		UPDATE_COLUMN();
		PARSE_ERROR_FMT("unexpected character '%s', expected hexdigit", format_char(ctx, fc));
		ctx->cs = angel_config_parser_error; fbreak;
	}

	action error_forbidden_character {
		UPDATE_COLUMN();
		PARSE_ERROR_FMT("forbidden character '%s' in string", format_char(ctx, fc));
		ctx->cs = angel_config_parser_error; fbreak;
	}

	action error_expected_semicolon {
		UPDATE_COLUMN();
		PARSE_ERROR_FMT("unexpected character '%s', expected ';'", format_char(ctx, fc));
		ctx->cs = angel_config_parser_error; fbreak;
	}

	action error_expected_colon {
		UPDATE_COLUMN();
		PARSE_ERROR_FMT("unexpected character '%s', expected ':'", format_char(ctx, fc));
		ctx->cs = angel_config_parser_error; fbreak;
	}

	action error_expected_string {
		UPDATE_COLUMN();
		PARSE_ERROR_FMT("unexpected character '%s', expected a string (\"...\")", format_char(ctx, fc));
		ctx->cs = angel_config_parser_error; fbreak;
	}

	action error_expected_number {
		UPDATE_COLUMN();
		PARSE_ERROR_FMT("unexpected character '%s', expected a number", format_char(ctx, fc));
		ctx->cs = angel_config_parser_error; fbreak;
	}

	action error_expected_value {
		UPDATE_COLUMN();
		PARSE_ERROR_FMT("unexpected character '%s', expected a value", format_char(ctx, fc));
		ctx->cs = angel_config_parser_error; fbreak;
	}

	action error_expected_hash_end {
		UPDATE_COLUMN();
		PARSE_ERROR_FMT("unexpected character '%s', expected ']'", format_char(ctx, fc));
		ctx->cs = angel_config_parser_error; fbreak;
	}

	action error_expected_list_end {
		UPDATE_COLUMN();
		PARSE_ERROR_FMT("unexpected character '%s', expected ')'", format_char(ctx, fc));
		ctx->cs = angel_config_parser_error; fbreak;
	}



	action string_start {
		g_string_truncate(ctx->token, 0);
	}

	action string_append {
		g_string_append_len(ctx->token, fpc, 1);
	}


	action value_true {
		ctx->curvalue = value_new_bool(TRUE);
	}

	action value_false {
		ctx->curvalue = value_new_bool(FALSE);
	}

	action value_number {
		ctx->curvalue = value_new_number(ctx->number);
	}

	action value_range {
		liValueRange vr = { ctx->number2, ctx->number };
		ctx->curvalue = value_new_range(vr);
		if (ctx->number2 > ctx->number) {
			GString *tmp = value_to_string(ctx->curvalue);
			UPDATE_COLUMN();
			PARSE_ERROR_FMT("range broken: %s (from > to)", tmp->str);
			g_string_free(tmp, TRUE);
			ctx->cs = angel_config_parser_error; fbreak;
		}
	}

	action value_string {
		ctx->curvalue = value_new_string(ctx->token);
		ctx->token = g_string_sized_new(0);
	}

	action value_list_start {
		g_ptr_array_add(ctx->valuestack, value_new_list());
		ctx->curvalue = NULL;
		fcall value_list_sub;
	}
	action value_list_push {
		liValue *vlist = g_ptr_array_index(ctx->valuestack, ctx->valuestack->len-1);
		g_ptr_array_add(vlist->data.list, ctx->curvalue);
		ctx->curvalue = NULL;
	}
	action value_list_end {
		guint ndx = ctx->valuestack->len - 1;
		ctx->curvalue = g_ptr_array_index(ctx->valuestack, ndx);
		g_ptr_array_set_size(ctx->valuestack, ndx);
		fret;
	}

	action value_hash_start {
		g_ptr_array_add(ctx->valuestack, value_new_hash());
		ctx->curvalue = NULL;
		fcall value_hash_sub;
	}
	action value_hash_push_name {
		g_ptr_array_add(ctx->valuestack, ctx->curvalue);
		ctx->curvalue = NULL;
	}
	action value_hash_push_value {
		guint ndx = ctx->valuestack->len-1;
		liValue *vname = g_ptr_array_index(ctx->valuestack, ndx);
		liValue *vhash = g_ptr_array_index(ctx->valuestack, ndx-1);
		g_ptr_array_set_size(ctx->valuestack, ndx);
		if (NULL != g_hash_table_lookup(vhash->data.hash, vname->data.string)) {
			UPDATE_COLUMN();
			PARSE_ERROR_FMT("duplicate key '%s' in item '%s'", vname->data.string->str, ctx->itemname->str);
			value_free(vname);
			ctx->cs = angel_config_parser_error; fbreak;
		}
		g_hash_table_insert(vhash->data.hash, vname->data.string, ctx->curvalue);
		ctx->curvalue = NULL;
		vname->type = LI_VALUE_NONE;
		value_free(vname);
	}
	action value_hash_end {
		guint ndx = ctx->valuestack->len - 1;
		ctx->curvalue = g_ptr_array_index(ctx->valuestack, ndx);
		g_ptr_array_set_size(ctx->valuestack, ndx);
		fret;
	}

	action option_start {
		g_ptr_array_add(ctx->valuestack, value_new_string(ctx->token));
		ctx->token = g_string_sized_new(0);
		ctx->curvalue= NULL;
	}
	action option_push {
		guint ndx = ctx->valuestack->len-1;
		liValue *vname = g_ptr_array_index(ctx->valuestack, ndx);
		g_ptr_array_set_size(ctx->valuestack, ndx);
		if (!ctx->curvalue) ctx->curvalue = value_new_none();
		if (NULL != g_hash_table_lookup(ctx->itemvalue->data.hash, vname->data.string)) {
			UPDATE_COLUMN();
			PARSE_ERROR_FMT("duplicate key '%s' in item '%s'", vname->data.string->str, ctx->itemname->str);
			value_free(vname);
			ctx->cs = angel_config_parser_error; fbreak;
		}
		g_hash_table_insert(ctx->itemvalue->data.hash, vname->data.string, ctx->curvalue);
		ctx->curvalue = NULL;
		vname->type = LI_VALUE_NONE;
		value_free(vname);
	}

	line_sane = ( '\n' ) @newline;
	line_weird = ( '\r' ) @newline;
	line_insane = ( '\r\n' ) @{ fctx->line--; };
	line = ( line_sane | line_weird | line_insane );
#	line = ( '\n' | '\r' '\n'? <: '' ) %newline;

	ws = [\t\v\f ];
	comment = '#' (any - line)* line;
	noise = ( ws | line | comment )+;
#   FIXME for viewing the statemachine
#	noise = ( ws | [\r\n] )+;

	id = (alpha (alnum | '.' | '-' | '_')** ) >starttoken %endtoken;

	special_chars = '\\' (
		  'n' @{ g_string_append(ctx->token, "\n"); }
		| 't' @{ g_string_append(ctx->token, "\t"); }
		| 'r' @{ g_string_append(ctx->token, "\r"); }
		| 'x'
			(xdigit @{ ctx->hexchar  = 16*g_ascii_xdigit_value(fc); }
			xdigit @{ ctx->hexchar +=    g_ascii_xdigit_value(fc);
			          g_string_append_len(ctx->token, &ctx->hexchar, 1);
			} ) @err(error_expected_xdigit)
		| (any - [ntrx] - cntrl) @string_append
		) ;
	string = ('"' @string_start ( (any - ["\\] - cntrl)@string_append | special_chars )* '"' ) <>err(error_forbidden_character);
#   FIXME for viewing the statemachine
#	string = '"' @string_start '"';

	action number_digit {
		if (ctx->negate) {
			if ((G_MININT64 + (fc - '0')) / 10 > ctx->number) {
				UPDATE_COLUMN();
				PARSE_ERROR("Integer underflow");
				ctx->cs = angel_config_parser_error; fbreak;
			}
			ctx->number = ctx->number*10 - (fc - '0');
		} else {
			if ((G_MAXINT64 - (fc - '0')) / 10 < ctx->number) {
				UPDATE_COLUMN();
				PARSE_ERROR("Integer overflow");
				ctx->cs = angel_config_parser_error; fbreak;
			}
			ctx->number = ctx->number*10 + (fc - '0');
		}
	}
	number = (('-'@{ctx->negate = TRUE;})? (digit digit**) $number_digit) >{ ctx->number = 0; ctx->negate = FALSE; };

	value_bool = ('true'i | 'enabled'i) %value_true | ('false'i | 'disabled'i) %value_false;
	value_number = number noise** ('-'@{ ctx->number2 = ctx->number; } (noise*) $err(error_expected_number) number %value_range | '' %value_number);
	value_string = string @value_string;
	value_list = '(' @value_list_start;
	value_hash = '[' @value_hash_start;
	value = (value_bool | value_number | value_string | value_list | value_hash) @err(error_expected_value);
#   FIXME for viewing the statemachine
#	value = "v";

	value_list_sub_item = value %value_list_push;
	value_list_sub := ((noise | value_list_sub_item noise* ',')* value_list_sub_item? (noise*) $err(error_expected_list_end) ')' @value_list_end) @err(error_unexpected_char);

	value_hash_sub_item = (value_string %value_hash_push_name) $err(error_expected_string) (noise*) $err(error_expected_colon) ':' noise* value %value_hash_push_value;
	value_hash_sub := ((noise | value_hash_sub_item noise* ',')* value_hash_sub_item? (noise*) $err(error_expected_hash_end) ']' @value_hash_end) @err(error_unexpected_char);

	option = id %option_start <: noise* value? (noise*) $err(error_expected_semicolon) ';' @option_push ;
	optionlist = ((noise | option) >err(error_expected_id) )*;
	item = (id %startitem) noise* ( ('{' optionlist '}') | ';' ) >err(error_expected_block) @enditem ;

	main := ((noise | item) >err(error_expected_id) <>err(error_unexpected_char) )*;
}%%

%% write data;

static void update_stack(pcontext *ctx) {
	g_array_set_size(ctx->g_stack, ctx->top + 1);
	ctx->stack = (int*) ctx->g_stack->data;
}

static int angel_config_parser_has_error(pcontext *ctx) {
	return ctx->cs == angel_config_parser_error;
}

static int angel_config_parser_is_finished(pcontext *ctx) {
	return ctx->cs >= angel_config_parser_first_final;
}

static pcontext* angel_config_parser_new() {
	pcontext *ctx = g_slice_new0(pcontext);
	ctx->g_stack = g_array_sized_new(FALSE, FALSE, sizeof(int), 8);
	ctx->top = 0;
	g_array_set_size(ctx->g_stack, ctx->top + 1);
	ctx->stack = (int*) ctx->g_stack->data;
	ctx->token = g_string_sized_new(0);
	ctx->valuestack = g_ptr_array_new();

	%% write init;

	return ctx;
}

static void angel_config_parser_free(pcontext *ctx) {
	if (!ctx) return;

	g_array_free(ctx->g_stack, TRUE);
	g_string_free(ctx->token, TRUE);
	for (guint i = 0; i < ctx->valuestack->len; i++) {
		value_free(g_ptr_array_index(ctx->valuestack, i));
	}
	g_ptr_array_free(ctx->valuestack, TRUE);
	if (ctx->itemname) g_string_free(ctx->itemname, TRUE);
	value_free(ctx->itemvalue);
	value_free(ctx->curvalue);
	g_slice_free(pcontext, ctx);
}

static gboolean angel_config_parser_finalize(pcontext *ctx, filecontext *fctx, GError **err) {
	if (!angel_config_parser_is_finished(ctx)) {
		PARSE_ERROR("unexpected end of file");
		return FALSE;
	}

	return TRUE;
}

static gboolean angel_config_parse_data(liServer *srv, pcontext *ctx, filecontext *fctx, gchar *data, gsize len, GError **err) {
	gchar *p = data, *pe = p+len, *eof = NULL, *linestart = p, *tokenstart = NULL;
	if (ctx->readingtoken) tokenstart = p;

	%% write exec;

	if (ctx->readingtoken) g_string_append_len(ctx->token, tokenstart, p - tokenstart);

	UPDATE_COLUMN();
	fctx->column--;

	if (err && *err) return FALSE;

	if (angel_config_parser_has_error(ctx)) {
		PARSE_ERROR("unknown parser error");
		return FALSE;
	}

	return TRUE;
}

gboolean angel_config_parse_file(liServer *srv, const gchar *filename, GError **err) {
	char *data = NULL;
	gsize len = 0;
	filecontext sfctx, *fctx = &sfctx;
	pcontext *ctx = angel_config_parser_new();

	if (err && *err) goto error;

	if (!g_file_get_contents(filename, &data, &len, err)) goto error;

	sfctx.filename = filename;
	sfctx.line = 1;
	sfctx.column = 1;
	if (!angel_config_parse_data(srv, ctx, fctx, data, len, err)) goto error;
	if (!angel_config_parser_finalize(ctx, fctx, err)) goto error;

	if (data) g_free(data);
	angel_config_parser_free(ctx);
	return TRUE;

error:
	if (data) g_free(data);
	angel_config_parser_free(ctx);
	return FALSE;
}

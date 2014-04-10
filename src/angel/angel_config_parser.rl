#include <lighttpd/angel_base.h>
#include <lighttpd/angel_config_parser.h>

#include <stdarg.h>

#define KV_LISTING_MAX 100

typedef enum {
	TK_ERROR,
	TK_EOF,
	TK_ASSIGN,
	TK_ASSOCICATE,
	TK_CAST_STRING,
	TK_CAST_INT,
	TK_COMMA,
	TK_CURLY_CLOSE,
	TK_CURLY_OPEN,
	TK_DEFAULT,
	TK_DIVIDE,
	TK_FALSE,
	TK_INTEGER,
	TK_MINUS,
	TK_MULTIPLY,
	TK_NAME,
	TK_NONE,
	TK_NOT,
	TK_PARA_CLOSE,
	TK_PARA_OPEN,
	TK_PLUS,
	TK_SEMICOLON,
	TK_SQUARE_CLOSE,
	TK_SQUARE_OPEN,
	TK_STRING,
	TK_TRUE
} liConfigToken;

typedef struct liConfigTokenizerContext liConfigTokenizerContext;
struct liConfigTokenizerContext {
	liServer *srv;

	/* ragel vars */
	int cs;
	const char *p, *pe, *eof;

	/* remember token start position for error messages */
	const char *token_start;
	gsize token_line;
	const gchar *token_line_start;

	/* mark start of strings and similar */
	const gchar *mark;

	/* number stuff */
	gboolean negative;
	gint64 suffix_factor;

	/* information about currently parsed file */
	const gchar *filename;
	const gchar *content; /* pointer to the data */
	gsize len;
	gsize line; /* holds current line */
	const gchar *line_start;

	/* result */
	GString *token_string;
	gint64 token_number;

	/* TK_ERROR => have to parse it. parsing returns the token, but you can put it back here */
	/* only put the last token back, as on the next parse call token_string and token_number get reset */
	liConfigToken next_token;

	GHashTable *variables;
};

static liConfigToken tokenizer_error(liConfigTokenizerContext *ctx, GError **error, const char *fmt, ...) G_GNUC_PRINTF(3, 4);

GQuark li_angel_config_parser_error_quark(void) {
	return g_quark_from_string("li-angel-config-parser-error-quark");
}

%%{
	## ragel stuff
	machine config_tokenizer;

	variable p ctx->p;
	variable pe ctx->pe;
	variable eof ctx->eof;

	access ctx->;

	action mark {
		ctx->mark = fpc;
	}

	action name {
		g_string_append_len(ctx->token_string, ctx->mark, fpc - ctx->mark);
		return TK_NAME;
	}

	action char {
		g_string_append_c(ctx->token_string, fc);
	}
	action echar {
		switch (fc) {
		case 't':
			g_string_append_c(ctx->token_string, '\t');
			break;
		case 'r':
			g_string_append_c(ctx->token_string, '\r');
			break;
		case 'n':
			g_string_append_c(ctx->token_string, '\n');
			break;
		case '"':
		case '\'':
		case '\\':
			g_string_append_c(ctx->token_string, fc);
			break;
		default:
			g_string_append_c(ctx->token_string, '\\');
			g_string_append_c(ctx->token_string, fc);
			break;
		}
	}
	action xchar {
		char xstr[3] = "  ";
		xstr[0] = fpc[-1]; xstr[1] = fpc[0];
		g_string_append_c(ctx->token_string, strtol(xstr, NULL, 16));
	}
	action string {
		++fpc;
		return TK_STRING;
	}

	action e_char {
		g_string_append_c(ctx->token_string, fc);
	}
	action e_echar {
		switch (fc) {
		case 't':
			g_string_append_c(ctx->token_string, '\t');
			break;
		case 'r':
			g_string_append_c(ctx->token_string, '\r');
			break;
		case 'n':
			g_string_append_c(ctx->token_string, '\n');
			break;
		default:
			g_string_append_c(ctx->token_string, fc);
			break;
		}
	}
	action e_xchar {
		char xstr[3] = "  ";
		xstr[0] = fpc[-1]; xstr[1] = fpc[0];
		g_string_append_c(ctx->token_string, strtol(xstr, NULL, 16));
	}
	action e_string {
		++fpc;
		return TK_STRING;
	}

	action int_start {
		ctx->negative = FALSE;
		ctx->token_number = 0;
		ctx->suffix_factor = 1;
	}
	action int_negative {
		ctx->negative = TRUE;
	}
	action dec_digit {
		gint64 digit = fc - '0';
		if (ctx->negative) {
			if (ctx->token_number < G_MININT64 / 10 + digit) {
				return tokenizer_error(ctx, error, "integer overflow");
			}
			ctx->token_number = 10*ctx->token_number - digit;
		} else {
			if (ctx->token_number > G_MAXINT64 / 10 - digit) {
				return tokenizer_error(ctx, error, "integer overflow");
			}
			ctx->token_number = 10*ctx->token_number + digit;
		}
	}
	action oct_digit {
		gint64 digit = fc - '0';
		if (ctx->negative) {
			if (ctx->token_number < G_MININT64 / 8 + digit) {
				return tokenizer_error(ctx, error, "integer overflow");
			}
			ctx->token_number = 8*ctx->token_number - digit;
		} else {
			if (ctx->token_number > G_MAXINT64 / 8 - digit) {
				return tokenizer_error(ctx, error, "integer overflow");
			}
			ctx->token_number = 8*ctx->token_number + digit;
		}
	}
	action hex_digit {
		gint64 digit;
		if (fc >= '0' && fc <= '9') digit = fc - '0';
		else if (fc >= 'a' && fc <= 'f') digit = fc - 'a' + 10;
		else /*if (fc >= 'A' && fc <= 'F')*/ digit = fc - 'A' + 10;
		if (ctx->negative) {
			if (ctx->token_number < G_MININT64 / 16 + digit) {
				return tokenizer_error(ctx, error, "integer overflow");
			}
			ctx->token_number = 16*ctx->token_number - digit;
		} else {
			if (ctx->token_number > G_MAXINT64 / 16 - digit) {
				return tokenizer_error(ctx, error, "integer overflow");
			}
			ctx->token_number = 16*ctx->token_number + digit;
		}
	}
	action integer_suffix {
		if (ctx->negative) {
			if (ctx->token_number < G_MININT64 / ctx->suffix_factor) {
				return tokenizer_error(ctx, error, "integer overflow in suffix");
			}
		} else if (ctx->token_number < 0) {
			if (ctx->token_number > G_MAXINT64 / ctx->suffix_factor) {
				return tokenizer_error(ctx, error, "integer overflow in suffix");
			}
		}
		ctx->token_number *= ctx->suffix_factor;
	}
	action integer {
		return TK_INTEGER;
	}

	noise_char = [\t \r\n#];
	operator_char = [+\-*/=<>!^$~;,(){}[\]] | '"' | "'";
	operator_separator_char = [;,(){}[\]];
	keywords = ( 'default' | 'cast' | 'false' | 'none' | 'not' | 'true' );


	line_sane = ( '\n' ) >{ ctx->line++; ctx->line_start = fpc + 1; };
	line_weird = ( '\r' ) >{ ctx->line++; ctx->line_start = fpc + 1; };
	line_insane = ( '\r' ( '\n' >{ ctx->line--; } ) );
	line = ( line_sane | line_weird | line_insane );

	ws = ( '\t' | ' ' );
	comment = ( '#' (any - line)* line );
	noise = ( ws | line | comment )**;

	integer_suffix_bytes =
		( 'byte'    %{ ctx->suffix_factor = G_GINT64_CONSTANT(1); }
		| 'kbyte'   %{ ctx->suffix_factor = G_GINT64_CONSTANT(1024); }
		| 'mbyte'   %{ ctx->suffix_factor = G_GINT64_CONSTANT(1024)*1024; }
		| 'gbyte'   %{ ctx->suffix_factor = G_GINT64_CONSTANT(1024)*1024*1024; }
		| 'tbyte'   %{ ctx->suffix_factor = G_GINT64_CONSTANT(1024)*1024*1024*1024; }
		| 'pbyte'   %{ ctx->suffix_factor = G_GINT64_CONSTANT(1024)*1024*1024*1024*1024; }
		);
	integer_suffix_bits =
		( 'bit'     %{ ctx->token_number /= 8; ctx->suffix_factor = G_GINT64_CONSTANT(1); }
		| 'kbit'    %{ ctx->suffix_factor = G_GINT64_CONSTANT(1024) / 8; }
		| 'mbit'    %{ ctx->suffix_factor = G_GINT64_CONSTANT(1024)*1024 / 8; }
		| 'gbit'    %{ ctx->suffix_factor = G_GINT64_CONSTANT(1024)*1024*1024 / 8; }
		| 'tbit'    %{ ctx->suffix_factor = G_GINT64_CONSTANT(1024)*1024*1024*1024 / 8; }
		| 'pbit'    %{ ctx->suffix_factor = G_GINT64_CONSTANT(1024)*1024*1024*1024*1024 / 8; }
		);
	integer_suffix_seconds =
		( 'sec'     %{ ctx->suffix_factor = G_GINT64_CONSTANT(1); }
		| 'min'     %{ ctx->suffix_factor = G_GINT64_CONSTANT(60); }
		| 'hours'   %{ ctx->suffix_factor = G_GINT64_CONSTANT(3600); }
		| 'days'    %{ ctx->suffix_factor = G_GINT64_CONSTANT(24)*3600; }
		);
	integer_suffix = ( integer_suffix_bytes | integer_suffix_bits | integer_suffix_seconds ) %integer_suffix;
	integer_base = ('-'@int_negative)?
		( ([1-9] [0-9]*)$dec_digit
		| '0'
		| '0' ([0-9]+)$oct_digit
		| '0x' (xdigit+)$hex_digit
		)>int_start;
	# noise_char only matches after integer_suffix, as noise matches all noice chars greedy
	integer = (integer_base noise integer_suffix?)%integer (noise_char | operator_char);

	string =
		( ( '"' ( (any - [\\"])@char | ('\\' [^x])@echar | ('\\x' xdigit{2})@xchar )* '"' ) @string
		| ( "'" ( (any - [\\'])@char | ('\\' [^x])@echar | ('\\x' xdigit{2})@xchar )* "'" ) @string
		| ( 'e"' ( (any - [\\"])@e_char | ('\\' [trn\\"'])@e_echar | ('\\x' xdigit{2})@e_xchar )* '"' ) @e_string
		| ( "e'" ( (any - [\\'])@e_char | ('\\' [trn\\"'])@e_echar | ('\\x' xdigit{2})@e_xchar )* "'" ) @e_string
		);

	keyword =
		( 'default'       %{ return TK_DEFAULT; }
		| 'false'         %{ return TK_FALSE; }
		| 'not'           %{ return TK_NOT; }
		| 'none'          %{ return TK_NONE; }
		| 'true'          %{ return TK_TRUE; }
		) %(identifier, 1) (noise_char | operator_char)?;

	name = ( (alpha | '_') (alnum | [._])* - keywords) >mark %name (noise_char | operator_char)?;

	cast =
		( 'cast' noise '(' noise 'int' noise ')'      @{ ++fpc; return TK_CAST_INT; }
		| 'cast' noise '(' noise 'string' noise ')'   @{ ++fpc; return TK_CAST_STRING; }
		);

	single_operator =
		( ';'    @{ ++fpc; return TK_SEMICOLON; }
		| ','    @{ ++fpc; return TK_COMMA; }
		| '('    @{ ++fpc; return TK_PARA_OPEN; }
		| ')'    @{ ++fpc; return TK_PARA_CLOSE; }
		| '['    @{ ++fpc; return TK_SQUARE_OPEN; }
		| ']'    @{ ++fpc; return TK_SQUARE_CLOSE; }
		| '{'    @{ ++fpc; return TK_CURLY_OPEN; }
		| '}'    @{ ++fpc; return TK_CURLY_CLOSE; }
		);

	operator =
		 # minus not accepted here in front of a digit
		  '-'    %{ return TK_MINUS; } (noise_char | operator_separator_char | alpha | '_' | '"' | "'")? |
		( '+'    %{ return TK_PLUS; }
		| '*'    %{ return TK_MULTIPLY; }
		| '/'    %{ return TK_DIVIDE; }
		| '!'    %{ return TK_NOT; }
		| '='    %{ return TK_ASSIGN; }
		| '=>'   %{ return TK_ASSOCICATE; }
		) (noise_char | operator_separator_char | alnum | '_' | '"' | "'")?;

	endoffile = '' %{ fpc = NULL; return TK_EOF; };

	token :=
		noise
		( endoffile | keyword | name | cast | single_operator | operator | integer | string );
}%%

%% write data;

static void set_config_error(liConfigTokenizerContext *ctx, GError **error, const char *fmt, va_list ap) {
	GString *msg = g_string_sized_new(127);

	g_string_vprintf(msg, fmt, ap);

	g_set_error(error,
		LI_ANGEL_CONFIG_PARSER_ERROR,
		LI_ANGEL_CONFIG_PARSER_ERROR_PARSE,
		"error in %s:%" G_GSIZE_FORMAT ":%" G_GSIZE_FORMAT ": %s",
		ctx->filename, ctx->token_line, 1 + ctx->token_start - ctx->token_line_start, msg->str);
	g_string_free(msg, TRUE);
}

static liConfigToken tokenizer_error(liConfigTokenizerContext *ctx, GError **error, const char *fmt, ...) {
	va_list ap;

	va_start(ap, fmt);
	set_config_error(ctx, error, fmt, ap);
	va_end(ap);

	return TK_ERROR;
}

static gboolean parse_error(liConfigTokenizerContext *ctx, GError **error, const char *fmt, ...) {
	va_list ap;

	va_start(ap, fmt);
	set_config_error(ctx, error, fmt, ap);
	va_end(ap);

	return FALSE;
}



static gboolean tokenizer_init_file(liServer *srv, liConfigTokenizerContext *ctx, const gchar *filename, GError **error) {
	memset(ctx, 0, sizeof(*ctx));
	ctx->srv = srv;

	if (!g_file_get_contents(filename, (gchar**) &ctx->content, &ctx->len, error)) return FALSE;

	ctx->p = ctx->content;
	ctx->pe = ctx->eof = ctx->content + ctx->len;

	ctx->filename = filename;
	ctx->line = 1;
	ctx->line_start = ctx->content;

	ctx->token_string = g_string_sized_new(31);

	ctx->variables = li_value_new_hashtable();

	return TRUE;
}

static void tokenizer_clear(liConfigTokenizerContext *ctx) {
	g_string_free(ctx->token_string, TRUE);
}

static liConfigToken tokenizer_next(liConfigTokenizerContext *ctx, GError **error) {
	g_string_truncate(ctx->token_string, 0);
	ctx->token_number = 0;

	if (NULL == ctx->p) return tokenizer_error(ctx, error, "already reached end of file");

	ctx->token_start = ctx->p;
	ctx->token_line = ctx->line;
	ctx->token_line_start = ctx->line_start;

	%% write init;

	%% write exec;

	return tokenizer_error(ctx, error, "couldn't parse token");
}



#define NEXT(token) do { \
		if (TK_ERROR != ctx->next_token) { token = ctx->next_token; ctx->next_token = TK_ERROR; } \
		else if (TK_ERROR == (token = tokenizer_next(ctx, error))) goto error; \
	} while (0)
#define REMEMBER(token) do { \
		LI_FORCE_ASSERT(TK_ERROR == ctx->next_token); /* mustn't contain a token */ \
		ctx->next_token = token; \
	} while (0)


/* copy name, takeover value */
static gboolean setvar(liConfigTokenizerContext *ctx, GString *name, liValue *value, GError **error) {
	if (g_str_has_prefix(name->str, "sys.")) {
		li_value_free(value);
		return parse_error(ctx, error, "sys.* variables are read only");
	}

	g_hash_table_insert(ctx->variables, g_string_new_len(GSTR_LEN(name)), value);
	return TRUE;
}

static liValue* getvar(liConfigTokenizerContext *ctx, GString *name, GError **error) {
	liValue *value;

	if (g_str_has_prefix(name->str, "sys.")) {
		if (g_str_equal(name->str, "sys.pid")) {
			return li_value_new_number(getpid());
		} else if (g_str_equal(name->str, "sys.cwd")) {
			gchar cwd[1024];

			if (NULL != getcwd(cwd, sizeof(cwd)-1)) {
				return li_value_new_string(g_string_new(cwd));
			} else {
				parse_error(ctx, error, "failed to get CWD: %s", g_strerror(errno));
				return NULL;
			}
		} else if (g_str_equal(name->str, "sys.version")) {
			return li_value_new_string(g_string_new(PACKAGE_VERSION));
		} else if (g_str_has_prefix(name->str, "sys.env.")) {
			/* look up string in environment, push value onto stack */
			gchar *env = getenv(name->str + sizeof("sys.env.") - 1);
			if (env == NULL) {
				parse_error(ctx, error, "undefined environment variable: %s", name->str + sizeof("sys.env.") - 1);
				return NULL;
			}
			return li_value_new_string(g_string_new(env));
		}
		parse_error(ctx, error, "unknown sys.* variable: %s", name->str);
		return NULL;
	}

	value = g_hash_table_lookup(ctx->variables, name);
	if (NULL != value) return li_value_copy(value);

	parse_error(ctx, error, "undefined variable '%s'", name->str);

	return NULL;
}


static gboolean p_config_call(GString *name, liConfigTokenizerContext *ctx, GError **error);
static gboolean p_config_calls(liConfigTokenizerContext *ctx, GError **error);
static gboolean p_value_list(gint *key_value_nesting, liValue **result, gboolean key_value_list, liValue *pre_value, liConfigToken end, liConfigTokenizerContext *ctx, GError **error);
static gboolean p_value_group(gint *key_value_nesting, liValue **value, liConfigTokenizerContext *ctx, GError **error);
static gboolean p_value(gint *key_value_nesting, liValue **value, liConfigToken preOp, liConfigTokenizerContext *ctx, GError **error);
static gboolean p_vardef(GString *name, liConfigTokenizerContext *ctx, GError **error);
static gboolean p_parameter_values(liValue **result, liConfigTokenizerContext *ctx, GError **error);

/* whether token can be start of a value */
static gboolean is_value_start_token(liConfigToken token) {
	switch (token) {
	case TK_CAST_STRING:
	case TK_CAST_INT:
	case TK_DEFAULT:
	case TK_FALSE:
	case TK_INTEGER:
	case TK_MINUS:
	case TK_NAME:
	case TK_NONE:
	case TK_NOT:
	case TK_PARA_OPEN:
	case TK_SQUARE_OPEN:
	case TK_STRING:
	case TK_TRUE:
		return TRUE;
	default:
		return FALSE;
	}
}


static int _op_precedence(liConfigToken op) {
	switch (op) {
	case TK_PLUS:
	case TK_MINUS:
		return 3;
	case TK_MULTIPLY:
	case TK_DIVIDE:
		return 4;
	default:
		return 0;
	}
}
/* returns TRUE iff a <op1> b <op2> c == (a <op1> b) <op2> c
 *   returns TRUE if <op2> isn't an operator
 *   returns FALSE if <op1> isn't an operator but <op2> is
 */
static gboolean operator_precedence(liConfigToken op1, liConfigToken op2) {
	return _op_precedence(op1) >= _op_precedence(op2);
}

/* overflow checks, assuming two-complement representation, i.e. G_MININT64 == (-G_MAXINT64)+  - 1, -G_MININT64 "==" G_MININT64 */
static gboolean overflow_op_plus_int(gint64 a, gint64 b) {
	if (a < 0) {
		if (b >= 0) return FALSE;
		return (a < G_MININT64 - b);
	} else {
		if (b <= 0) return FALSE;
		return (a > G_MAXINT64 - b);
	}
}

static gboolean overflow_op_minus_int(gint64 a, gint64 b) {
	if (a < 0) {
		if (b <= 0) return FALSE;
		return (a < G_MININT64 + b);
	} else {
		if (b >= 0) return FALSE;
		return (a > G_MAXINT64 + b);
	}
}

static gboolean overflow_op_multiply_int(gint64 a, gint64 b) {
	if (0 == a || 0 == b) return FALSE;
	if (a < 0) {
		if (b > 0) return (a < G_MININT64 / b);
		if (G_MININT64 == a || G_MININT64 == b) return TRUE;
		/* b < 0 */ return ((-a) > G_MAXINT64 / (-b));
	} else { /* a > 0 */
		if (b < 0) return (b < G_MININT64 / a);
		/* b > 0 */ return (a > G_MAXINT64 / b);
	}
}

static gboolean overflow_op_divide_int(gint64 a, gint64 b) {
	/* only MIN INT / -1 == MAX INT + 1 overflows */
	return (-1 == b && G_MININT64 == a);
}

/* always frees v1 and v2 */
static gboolean op_execute(liValue **vresult, liConfigToken op, liValue *v1, liValue *v2, liConfigTokenizerContext *ctx, GError **error) {
	if (li_value_type(v1) == li_value_type(v2)) {
		switch (li_value_type(v1)) {
		case LI_VALUE_NUMBER:
			switch (op) {
			case TK_PLUS:
				if (overflow_op_plus_int(v1->data.number, v2->data.number)) {
					parse_error(ctx, error, "overflow in addition");
					goto error;
				}
				*vresult = li_value_new_number(v1->data.number + v2->data.number);
				break;
			case TK_MINUS:
				if (overflow_op_minus_int(v1->data.number, v2->data.number)) {
					parse_error(ctx, error, "overflow in subtraction");
					goto error;
				}
				*vresult = li_value_new_number(v1->data.number - v2->data.number);
				break;
			case TK_MULTIPLY:
				if (overflow_op_multiply_int(v1->data.number, v2->data.number)) {
					parse_error(ctx, error, "overflow in product");
					goto error;
				}
				*vresult = li_value_new_number(v1->data.number + v2->data.number);
				break;
			case TK_DIVIDE:
				if (0 == v2->data.number) {
					parse_error(ctx, error, "divide by zero");
					goto error;
				}
				if (overflow_op_divide_int(v1->data.number, v2->data.number)) {
					parse_error(ctx, error, "overflow in quotient");
					goto error;
				}
				*vresult = li_value_new_number(v1->data.number + v2->data.number);
				break;
			default:
				parse_error(ctx, error, "unsupported operation on numbers");
				goto error;
			}
			break;
		case LI_VALUE_LIST:
			switch (op) {
			case TK_PLUS:
				*vresult = li_value_extract(v1);
				LI_VALUE_FOREACH(entry, v2)
					li_value_list_append(*vresult, li_value_extract(entry));
				LI_VALUE_END_FOREACH()
				break;
			default:
				parse_error(ctx, error, "unsupported operation on lists");
				goto error;
			}
			break;
		case LI_VALUE_STRING:
			switch (op) {
			case TK_PLUS:
				g_string_append_len(v1->data.string, GSTR_LEN(v2->data.string));
				*vresult = v1;
				v1 = NULL;
				break;
			default:
				parse_error(ctx, error, "unsupported operation on strings");
				goto error;
			}
			break;
		default:
			parse_error(ctx, error, "operations (+,-,*,/) on %s not supported", li_value_type_string(v1));
			goto error;
		}
	} else {
		parse_error(ctx, error, "operations (+,-,*,/) on mixed value types not allowed");
		goto error;
	}

	li_value_free(v1);
	li_value_free(v2);
	return TRUE;

error:
	li_value_free(v1);
	li_value_free(v2);
	return FALSE;
}

static gboolean p_config_call(GString *name, liConfigTokenizerContext *ctx, GError **error) {
	liValue *parameters = NULL;
	gboolean res;

	if (!p_parameter_values(&parameters, ctx, error)) return FALSE;

	if (g_str_equal(name->str, "__print")) {
		GString *s = li_value_to_string(parameters);
		DEBUG(ctx->srv, "config __print: %s", s->str);
		g_string_free(s, TRUE);
		li_value_free(parameters);
		return TRUE;
	}

	res = li_plugins_handle_item(ctx->srv, name, parameters, error);
	li_value_free(parameters);

	return res;
}

/* parse actions until EOF (if !block) or '}' (if block, TK_CURLY_CLOSE) */
static gboolean p_config_calls(liConfigTokenizerContext *ctx, GError **error) {
	liConfigToken token;
	GString *name = NULL;

	NEXT(token);
	switch (token) {
	case TK_NAME:
		name = g_string_new_len(GSTR_LEN(ctx->token_string));
		NEXT(token);
		switch (token) {
		case TK_ASSIGN:
			if (!p_vardef(name, ctx, error)) goto error;
			break;
		default:
			REMEMBER(token);
			if (!p_config_call(name, ctx, error)) goto error;
			break;
		}
		g_string_free(name, TRUE); name = NULL;
		break;
	case TK_EOF:
		return TRUE;
	default:
		return parse_error(ctx, error, "unexpected token; expected action");
	}

	return p_config_calls(ctx, error);

error:
	if (NULL != name) g_string_free(name, TRUE); name = NULL;
	return FALSE;
}

static gboolean p_value_list(gint *key_value_nesting, liValue **result, gboolean key_value_list, liValue *pre_value, liConfigToken end, liConfigTokenizerContext *ctx, GError **error) {
	liValue *list = li_value_new_list();
	gint kv_nesting = *key_value_nesting;
	liValue *value = NULL, *key = NULL;

	for (;;) {
		gint sub_kv_nesting;
		liConfigToken token;

		if (NULL == pre_value) {
			NEXT(token);
			if (end == token) break;
			if (TK_COMMA == token) continue;
			REMEMBER(token);

			if (!p_value(&sub_kv_nesting, &value, TK_ERROR, ctx, error)) goto error;
			if (sub_kv_nesting < kv_nesting) kv_nesting = sub_kv_nesting;
		} else {
			value = pre_value;
			pre_value = NULL;
		}

		NEXT(token);
		if (!key_value_list && TK_ASSOCICATE == token) {
			key_value_list = TRUE;
			if (li_value_list_len(list) > 0) {
				return parse_error(ctx, error, "unexpected '=>'");
				goto error;
			}
		}
		if (key_value_list) {
			liValue *pair;

			if (TK_ASSOCICATE != token) {
				parse_error(ctx, error, "expected '=>'");
				goto error;
			}
			key = value;
			value = NULL;
			if (!p_value(&sub_kv_nesting, &value, TK_ERROR, ctx, error)) goto error;

			pair = li_value_new_list();
			li_value_list_append(pair, key);
			li_value_list_append(pair, value);
			li_value_list_append(list, pair);
			value = key = NULL;
		} else {
			REMEMBER(token);
			li_value_list_append(list, value);
			value = NULL;
		}
	}

	if (key_value_list) *key_value_nesting = 0;
	else *key_value_nesting = kv_nesting + 1;
	*result = list;
	return TRUE;

error:
	li_value_free(list);
	li_value_free(value);
	li_value_free(key);
	return FALSE;
}

static gboolean p_value_group(gint *key_value_nesting, liValue **value, liConfigTokenizerContext *ctx, GError **error) {
	liValue *v = NULL;
	liConfigToken token;

	if (!p_value(key_value_nesting, &v, TK_ERROR, ctx, error)) return FALSE;

	NEXT(token);
	switch (token) {
	case TK_PARA_CLOSE:
		*value = v;
		return TRUE;
	case TK_COMMA:
		/* a list */
		REMEMBER(token);
		return p_value_list(key_value_nesting, value, FALSE, v, TK_PARA_CLOSE, ctx, error);
	case TK_ASSOCICATE:
		/* a key-value list */
		REMEMBER(token);
		return p_value_list(key_value_nesting, value, TRUE, v, TK_PARA_CLOSE, ctx, error);
	default:
		li_value_free(v);
		return parse_error(ctx, error, "expected ')'");
	}

error:
	li_value_free(v);
	return FALSE;
}

/* preOp: TK_ERROR - no operator before value
 *        TK_NOT - unary operator before value (not, casts)
 *        or the binary operator before value
 */
static gboolean p_value(gint *key_value_nesting, liValue **value, liConfigToken preOp, liConfigTokenizerContext *ctx, GError **error) {
	liConfigToken token;
	liValue *v = NULL, *vcast;
	gint kv_nesting = KV_LISTING_MAX;

	NEXT(token);
	if (!is_value_start_token(token)) return parse_error(ctx, error, "expected value");
	switch (token) {
	case TK_CAST_STRING: {
			GString *s;

			if (!p_value(&kv_nesting, &v, TK_NOT, ctx, error)) return FALSE;
			s = li_value_to_string(v);
			if (NULL == s) s = g_string_sized_new(0);
			li_value_free(v);
			v = li_value_new_string(s);
			break;
		}
		break;
	case TK_CAST_INT:
		if (!p_value(&kv_nesting, &v, TK_NOT, ctx, error)) return FALSE;
		switch (li_value_type(v)) {
		case LI_VALUE_NUMBER:
			break; /* nothing to do */
		case LI_VALUE_BOOLEAN:
			vcast = v;
			v = li_value_new_number(vcast->data.boolean ? 1 : 0);
			li_value_free(vcast);
			break;
		case LI_VALUE_STRING: {
				GString *s = v->data.string;
				gchar *endptr = NULL;
				gint64 i;

				errno = 0;
				i = g_ascii_strtoll(s->str, &endptr, 10);
				if (errno != 0 || endptr != s->str + s->len || s->len == 0) {
					li_value_free(v);
					return parse_error(ctx, error, "cast(int) failed, not a valid number: '%s'", s->str);
				}
				li_value_free(v);
				v = li_value_new_number(i);
			}
		default:
			parse_error(ctx, error, "cast(int) from %s not supported yet", li_value_type_string(v));
			li_value_free(v);
			return FALSE;
		}
		break;
	case TK_DEFAULT:
		v = li_value_new_none();
		break;
	case TK_FALSE:
		v = li_value_new_bool(FALSE);
		break;
	case TK_INTEGER:
		v = li_value_new_number(ctx->token_number);
		break;
	case TK_MINUS:
		if (!p_value(&kv_nesting, &v, TK_NOT, ctx, error)) return FALSE;
		if (li_value_type(v) == LI_VALUE_NUMBER) {
			if (v->data.number == G_MININT64) {
				li_value_free(v);
				return parse_error(ctx, error, "'-' overflow on minimum int value");
			}
			v->data.number = -v->data.number;
		} else {
			li_value_free(v);
			return parse_error(ctx, error, "'-' only supported for integer values");
		}
		break;
	case TK_NAME:
		v = getvar(ctx, ctx->token_string, error);
		if (NULL == v) return FALSE;
		break;
	case TK_NONE:
		v = li_value_new_none();
		break;
	case TK_NOT:
		if (!p_value(&kv_nesting, &v, TK_NOT, ctx, error)) return FALSE;
		if (li_value_type(v) == LI_VALUE_BOOLEAN) {
			v->data.boolean = !v->data.boolean;
		} else {
			li_value_free(v);
			return parse_error(ctx, error, "'not' only supported for boolean values");
		}
		break;
	case TK_PARA_OPEN:
		if (!p_value_group(&kv_nesting, &v, ctx, error)) return FALSE;
		break;
	case TK_SQUARE_OPEN:
		if (!p_value_list(&kv_nesting, &v, FALSE, NULL, TK_SQUARE_CLOSE, ctx, error)) return FALSE;
		break;
	case TK_STRING:
		v = li_value_new_string(g_string_new_len(GSTR_LEN(ctx->token_string)));
		break;
	case TK_TRUE:
		v = li_value_new_bool(TRUE);
		break;
	default:
		/* is_value_start_token should have been false */
		return parse_error(ctx, error, "internal error: unsupported value type");
	}

	if (preOp == TK_NOT) {
		/* had unary operator before it */
		*value = v;
		*key_value_nesting = kv_nesting;
		return TRUE;
	}

	for (;;) {
		liValue *v2 = NULL, *vresult = NULL;
		gint sub_kv_nesting = KV_LISTING_MAX;

		NEXT(token);
		if (operator_precedence(preOp, token)) {
			REMEMBER(token);
			*value = v;
			*key_value_nesting = kv_nesting;
			return TRUE;
		}

		if (!p_value(&sub_kv_nesting, &v2, token, ctx, error)) {
			li_value_free(v);
			return FALSE;
		}
		if (sub_kv_nesting < kv_nesting) kv_nesting = sub_kv_nesting;

		if (!op_execute(&vresult, token, v, v2, ctx, error)) return FALSE;
		v = vresult;
	}

error:
	li_value_free(v);
	return FALSE;
}

static gboolean p_parameter_values(liValue **result, liConfigTokenizerContext *ctx, GError **error) {
	gint key_value_nesting = KV_LISTING_MAX;
	liValue *value = NULL;
	liConfigToken token;

	NEXT(token);
	if (token == TK_SEMICOLON) {
		*result = NULL;
		return TRUE;
	}
	REMEMBER(token);

	if (!p_value(&key_value_nesting, &value, TK_ERROR, ctx, error)) return FALSE;

	NEXT(token);
	switch (token) {
	case TK_SEMICOLON:
		break;
	case TK_COMMA:
		/* a list */
		REMEMBER(token);
		if (!p_value_list(&key_value_nesting, &value, FALSE, value, TK_SEMICOLON, ctx, error)) return FALSE;
		break;
	case TK_ASSOCICATE:
		/* a key-value list */
		REMEMBER(token);
		if (!p_value_list(&key_value_nesting, &value, TRUE, value, TK_SEMICOLON, ctx, error)) return FALSE;
		break;
	default:
		li_value_free(value);
		return parse_error(ctx, error, "expected ';'");
	}

	/* goal: return a list of parameters. a key-value list should be interpreted as a single value, and not
	 * get split into parameters.
	 */
	switch (key_value_nesting) {
	case 0:
		/* outer key-value list, wrap once */
		li_value_wrap_in_list(value);
		break;
	default:
		/* always have a list of parameters */
		if (LI_VALUE_LIST != li_value_type(value)) li_value_wrap_in_list(value);
		break;
	}

/*
	{
		GString *s = li_value_to_string(value);
		DEBUG(ctx->srv, "parameters: %s", s->str);
		g_string_free(s, TRUE);
	}
	DEBUG(ctx->srv, "nested kv: %i", key_value_nesting);
*/

	*result = value;
	return TRUE;

error:
	li_value_free(value);
	return FALSE;
}


static gboolean p_vardef(GString *name, liConfigTokenizerContext *ctx, GError **error) {
	liValue *value = NULL;
	gint key_value_nesting;
	liConfigToken token;

	if (!p_value(&key_value_nesting, &value, TK_ERROR, ctx, error)) return FALSE;
	NEXT(token);
	if (TK_SEMICOLON != token) {
		li_value_free(value);
		return parse_error(ctx, error, "expected ';'");
	}

	return setvar(ctx, name, value, error);

error:
	li_value_free(value);
	return FALSE;
}

gboolean li_angel_config_parse_file(liServer *srv, const gchar *filename, GError **err) {
	liConfigTokenizerContext ctx;
	gboolean result;

	if (NULL != err && NULL != *err) return FALSE;

	if (!tokenizer_init_file(srv, &ctx, filename, err)) return FALSE;

	result = p_config_calls(&ctx, err);
	g_free((gchar*) ctx.content);
	tokenizer_clear(&ctx);

	return result;
}

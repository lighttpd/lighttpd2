#include <lighttpd/base.h>
#include <lighttpd/config_parser.h>

#ifdef HAVE_LUA_H
# include <lighttpd/config_lua.h>
#endif

#include <stdarg.h>

#define KV_LISTING_MAX 100

typedef enum {
	TK_ERROR,
	TK_EOF,
	TK_AND,
	TK_ASSIGN,
	TK_ASSOCICATE,
	TK_CAST_STRING,
	TK_CAST_INT,
	TK_COMMA,
	TK_CURLY_CLOSE,
	TK_CURLY_OPEN,
	TK_DEFAULT,
	TK_DIVIDE,
	TK_ELSE,
	TK_EQUAL,
	TK_FALSE,
	TK_GLOBAL,
	TK_GREATER,
	TK_GREATER_EQUAL,
	TK_IF,
	TK_INCLUDE,
	TK_INCLUDE_LUA,
	TK_INCLUDE_SHELL,
	TK_INTEGER,
	TK_LESS,
	TK_LESS_EQUAL,
	TK_LOCAL,
	TK_MATCH,
	TK_MINUS,
	TK_MULTIPLY,
	TK_NAME,
	TK_NONE,
	TK_NOT,
	TK_NOT_EQUAL,
	TK_NOT_MATCH,
	TK_NOT_PREFIX,
	TK_NOT_SUBNET,
	TK_NOT_SUFFIX,
	TK_OR,
	TK_PARA_CLOSE,
	TK_PARA_OPEN,
	TK_PLUS,
	TK_PREFIX,
	TK_SEMICOLON,
	TK_SETUP,
	TK_SQUARE_CLOSE,
	TK_SQUARE_OPEN,
	TK_STRING,
	TK_SUBNET,
	TK_SUFFIX,
	TK_TRUE
} liConfigToken;

typedef struct liConfigScope liConfigScope;
struct liConfigScope {
	liConfigScope *parent;

	GHashTable *variables;
};

typedef struct liConfigTokenizerContext liConfigTokenizerContext;
struct liConfigTokenizerContext {
	liServer *srv;
	liWorker *wrk;
	gboolean master_config; /* whether includes/changing global vars are allowed */

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

	/* variable storage */
	liConfigScope *current_scope;
};

typedef struct liConditionTree liConditionTree;
struct liConditionTree {
	gboolean negated;

	liCondition *condition;

	liConfigToken op;
	liConditionTree *left, *right;
};

static liConfigToken tokenizer_error(liConfigTokenizerContext *ctx, GError **error, const char *fmt, ...) G_GNUC_PRINTF(3, 4);

GQuark li_config_error_quark(void) {
	return g_quark_from_string("li-config-error-quark");
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
	keywords = ( 'and' | 'default' | 'else' | 'false' | 'global' | 'if' | 'include' | 'include_lua' | 'include_shell' | 'local' | 'none' | 'not' | 'or' | 'setup' | 'true' );


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
		( 'and'           %{ return TK_AND; }
		| 'default'       %{ return TK_DEFAULT; }
		| 'else'          %{ return TK_ELSE; }
		| 'false'         %{ return TK_FALSE; }
		| 'global'        %{ return TK_GLOBAL; }
		| 'if'            %{ return TK_IF; }
		| 'include'       %{ return TK_INCLUDE; }
		| 'include_lua'   %{ return TK_INCLUDE_LUA; }
		| 'include_shell' %{ return TK_INCLUDE_SHELL; }
		| 'local'         %{ return TK_LOCAL; }
		| 'none'          %{ return TK_NONE; }
		| 'not'           %{ return TK_NOT; }
		| 'or'            %{ return TK_OR; }
		| 'setup'         %{ return TK_SETUP; }
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
		| '=='   %{ return TK_EQUAL; }
		| '!='   %{ return TK_NOT_EQUAL; }
		| '=^'   %{ return TK_PREFIX; }
		| '!^'   %{ return TK_NOT_PREFIX; }
		| '=$'   %{ return TK_SUFFIX; }
		| '!$'   %{ return TK_NOT_SUFFIX; }
		| '<'    %{ return TK_LESS; }
		| '<='   %{ return TK_LESS_EQUAL; }
		| '>'    %{ return TK_GREATER; }
		| '>='   %{ return TK_GREATER_EQUAL; }
		| '=~'   %{ return TK_MATCH; }
		| '!~'   %{ return TK_NOT_MATCH; }
		| '=/'   %{ return TK_SUBNET; }
		| '!/'   %{ return TK_NOT_SUBNET; }
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

	g_set_error(error, LI_CONFIG_ERROR, 0, "error in %s:%" G_GSIZE_FORMAT ":%" G_GSIZE_FORMAT ": %s", ctx->filename, ctx->token_line, 1 + ctx->token_start - ctx->token_line_start, msg->str);
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



static void tokenizer_init(liServer *srv, liWorker *wrk, liConfigTokenizerContext *ctx, const gchar *filename, const gchar *content, gsize len) {
	memset(ctx, 0, sizeof(*ctx));
	ctx->srv = srv;
	ctx->wrk = wrk;
	ctx->p = content;
	ctx->pe = ctx->eof = content + len;

	ctx->filename = filename;
	ctx->content = content;
	ctx->len = len;
	ctx->line = 1;
	ctx->line_start = ctx->content;

	ctx->token_string = g_string_sized_new(31);
}

static gboolean tokenizer_init_file(liServer *srv, liWorker *wrk, liConfigTokenizerContext *ctx, const gchar *filename, GError **error) {
	memset(ctx, 0, sizeof(*ctx));
	ctx->srv = srv;
	ctx->wrk = wrk;
	if (!g_file_get_contents(filename, (gchar**) &ctx->content, &ctx->len, error)) return FALSE;

	ctx->p = ctx->content;
	ctx->pe = ctx->eof = ctx->content + ctx->len;

	ctx->filename = filename;
	ctx->line = 1;
	ctx->line_start = ctx->content;

	ctx->token_string = g_string_sized_new(31);

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
		assert(TK_ERROR == ctx->next_token); /* mustn't contain a token */ \
		ctx->next_token = token; \
	} while (0)


static void scope_enter(liConfigTokenizerContext *ctx) {
	liConfigScope *scope = g_slice_new0(liConfigScope);
	scope->parent = ctx->current_scope;
	scope->variables = li_value_new_hashtable();
	ctx->current_scope = scope;
}

static void scope_leave(liConfigTokenizerContext *ctx) {
	liConfigScope *scope = ctx->current_scope;
	assert(NULL != scope);
	ctx->current_scope = scope->parent;

	g_hash_table_destroy(scope->variables);

	scope->variables = NULL;
	scope->parent = NULL;
	g_slice_free(liConfigScope, scope);
}

/* copy name, takeover value */
static gboolean scope_setvar(liConfigTokenizerContext *ctx, GString *name, liValue *value, GError **error) {
	liConfigScope *scope;

	if (g_str_has_prefix(name->str, "sys.")) {
		li_value_free(value);
		return parse_error(ctx, error, "sys.* variables are read only");
	}

	scope = ctx->current_scope;
	/* search whether name already exists in a scope. otherwise make it local */
	while (NULL != scope) {
		if (NULL != g_hash_table_lookup(scope->variables, name)) {
			g_hash_table_insert(scope->variables, g_string_new_len(GSTR_LEN(name)), value);
			return TRUE;
		}
		scope = scope->parent;
	}

	if (ctx->master_config) {
		/* modify global vars in master config */
		if (NULL != ctx->srv->config_global_vars && NULL != g_hash_table_lookup(ctx->srv->config_global_vars, name)) {
			g_hash_table_insert(ctx->srv->config_global_vars, g_string_new_len(GSTR_LEN(name)), value);
			return TRUE;
		}
	}

	if (NULL != ctx->current_scope) {
		g_hash_table_insert(ctx->current_scope->variables, g_string_new_len(GSTR_LEN(name)), value);
		return TRUE;
	}

	/* no scope... do nothing */
	li_value_free(value);
	return parse_error(ctx, error, "can't write variable without scope");
}

/* copy name, takeover value */
static gboolean scope_local_setvar(liConfigTokenizerContext *ctx, GString *name, liValue *value, GError **error) {
	if (g_str_has_prefix(name->str, "sys.")) {
		li_value_free(value);
		return parse_error(ctx, error, "sys.* variables are read only");
	}

	if (NULL != ctx->current_scope) {
		g_hash_table_insert(ctx->current_scope->variables, g_string_new_len(GSTR_LEN(name)), value);
		return TRUE;
	}

	/* no scope... do nothing */
	li_value_free(value);
	return parse_error(ctx, error, "can't write variable without scope");
}

/* copy name, takeover value */
static gboolean scope_global_setvar(liConfigTokenizerContext *ctx, GString *name, liValue *value, GError **error) {
	if (g_str_has_prefix(name->str, "sys.")) {
		li_value_free(value);
		return parse_error(ctx, error, "sys.* variables are read only");
	}

	if (!ctx->master_config) {
		li_value_free(value);
		return parse_error(ctx, error, "modifying global variables not allowed in this context");
	}

	if (NULL == ctx->srv->config_global_vars) {
		li_value_free(value);
		return parse_error(ctx, error, "no global variable scope");
	}

	g_hash_table_insert(ctx->srv->config_global_vars, g_string_new_len(GSTR_LEN(name)), value);
	return TRUE;
}

/* read-only pointer, no reference, return NULL if not found - no error */
/* only to read "action" variables */
static liValue* scope_peekvar(liConfigTokenizerContext *ctx, GString *name) {
	liConfigScope *scope = ctx->current_scope;
	liValue *value;

	/* can't peek sys. vars */
	if (g_str_has_prefix(name->str, "sys.")) return NULL;

	while (NULL != scope) {
		if (NULL != (value = g_hash_table_lookup(scope->variables, name))) {
			return value;
		}
		scope = scope->parent;
	}

	if (NULL != ctx->srv->config_global_vars && NULL != (value = g_hash_table_lookup(ctx->srv->config_global_vars, name))) {
		return value;
	}

	return NULL;
}

static liValue* scope_getvar(liConfigTokenizerContext *ctx, GString *name, GError **error) {
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

	value = scope_peekvar(ctx, name);
	if (NULL != value) return li_value_copy(value);

	parse_error(ctx, error, "undefined variable '%s'", name->str);

	return NULL;
}


static gboolean p_include(liAction *list, liConfigTokenizerContext *ctx, GError **error);
static gboolean p_include_lua(liAction *list, liConfigTokenizerContext *ctx, GError **error);
static gboolean p_include_shell(liAction *list, liConfigTokenizerContext *ctx, GError **error);
static gboolean p_setup(GString *name, liConfigTokenizerContext *ctx, GError **error);
static gboolean p_setup_block(liConfigTokenizerContext *ctx, GError **error);
static gboolean p_action(liAction *list, GString *name, liConfigTokenizerContext *ctx, GError **error);
static gboolean p_actions(gboolean block, liAction *list, liConfigTokenizerContext *ctx, GError **error);
static gboolean p_value_list(gint *key_value_nesting, liValue **result, gboolean key_value_list, liValue *pre_value, liConfigToken end, liConfigTokenizerContext *ctx, GError **error);
static gboolean p_value_group(gint *key_value_nesting, liValue **value, liConfigTokenizerContext *ctx, GError **error);
static gboolean p_value(gint *key_value_nesting, liValue **value, liConfigToken preOp, liConfigTokenizerContext *ctx, GError **error);
static gboolean p_vardef(GString *name, int normalLocalGlobal, liConfigTokenizerContext *ctx, GError **error);
static gboolean p_parameter_values(liValue **result, liConfigTokenizerContext *ctx, GError **error);

static liAction* cond_walk(liServer *srv, liConditionTree *tree, liAction *positive, liAction *negative);
static gboolean p_condition_value(liConditionTree **cond, liConfigTokenizerContext *ctx, GError **error);
static gboolean p_condition_expr(liConditionTree **tree, liConfigToken preOp, liConfigTokenizerContext *ctx, GError **error);
static gboolean p_condition(liAction *list, liConfigTokenizerContext *ctx, GError **error);

/* whether token can be start of a value */
static gboolean is_value_start_token(liConfigToken token) {
	switch (token) {
	case TK_CAST_STRING:
	case TK_CAST_INT:
	case TK_CURLY_OPEN:
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
	case TK_AND:
		return 1;
	case TK_OR:
		return 2;
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
			case TK_AND:
			case TK_OR:
				parse_error(ctx, error, "boolean operations not allowed on numbers");
				goto error;
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
				*vresult = li_value_new_list();
				LI_VALUE_FOREACH(entry, v1)
					li_value_list_append(*vresult, li_value_extract(entry));
				LI_VALUE_END_FOREACH()
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

static gboolean include_file(liAction *list, GString *filename, liConfigTokenizerContext *ctx, GError **error) {
	liConfigTokenizerContext subctx;
	gboolean result;

	if (!tokenizer_init_file(ctx->srv, ctx->wrk, &subctx, filename->str, error)) {
		return FALSE;
	}

	subctx.master_config = ctx->master_config;

	scope_enter(&subctx);
	subctx.current_scope->parent = ctx->current_scope;
	result = p_actions(FALSE, list, &subctx, error);
	scope_leave(&subctx);
	g_free((gchar*) subctx.content);
	tokenizer_clear(&subctx);

	return result;
}

static gboolean p_include(liAction *list, liConfigTokenizerContext *ctx, GError **error) {
	liValue *parameters = NULL;
	liValue *val_fname;
	GString *fname;

	if (!p_parameter_values(&parameters, ctx, error)) return FALSE;

	val_fname = li_value_get_single_argument(parameters);
	if (NULL == val_fname || LI_VALUE_STRING != li_value_type(val_fname)) {
		parse_error(ctx, error, "include directive takes a string as parameter");
		goto error;
	}
	fname = val_fname->data.string;

	if (!ctx->master_config) {
		parse_error(ctx, error, "include not allowed");
		goto error;
	}

	if (NULL == strchr(fname->str, '*') && NULL == strchr(fname->str, '?')) {
		if (!include_file(list, fname, ctx, error)) goto error;
	} else {
		gchar *separator;
		GPatternSpec *pattern;
		gsize basedir_len;
		GDir *dir;
		const gchar *filename;
		GError *err = NULL;

		if (NULL == (separator = strrchr(fname->str, G_DIR_SEPARATOR))) {
			pattern = g_pattern_spec_new(fname->str);
			g_string_assign(fname, ".");
		} else {
			pattern = g_pattern_spec_new(separator + 1);
			g_string_truncate(fname, separator - fname->str);
		}

		dir = g_dir_open(fname->str, 0, &err);
		if (NULL == dir) {
			parse_error(ctx, error, "include couldn't open directory: %s", err->message);
			g_error_free(err);
			goto error;
		}
		g_string_append_c(fname, G_DIR_SEPARATOR);
		basedir_len = fname->len;

		/* loop through all filenames in the directory and include matching ones */
		while (NULL != (filename = g_dir_read_name(dir))) {
			if (!g_pattern_match_string(pattern, filename))
				continue;
			g_string_truncate(fname, basedir_len);
			g_string_append(fname, filename);

			if (!include_file(list, fname, ctx, error)) {
				g_pattern_spec_free(pattern);
				g_dir_close(dir);
				goto error;
			}
		}

		g_pattern_spec_free(pattern);
		g_dir_close(dir);
	}

	li_value_free(parameters);
	return TRUE;

error:
	li_value_free(parameters);
	return FALSE;
}

static gboolean p_include_lua(liAction *list, liConfigTokenizerContext *ctx, GError **error) {
	liValue *parameters = NULL;
	liValue *val_fname;
	liAction *a;

	if (!p_parameter_values(&parameters, ctx, error)) return FALSE;

	val_fname = li_value_get_single_argument(parameters);
	if (NULL == val_fname || LI_VALUE_STRING != li_value_type(val_fname)) {
		li_value_free(parameters);
		return parse_error(ctx, error, "include_lua directive takes a string as parameter");
	}

	if (!ctx->master_config) {
		li_value_free(parameters);
		return parse_error(ctx, error, "include_lua not allowed");
	}

#ifdef HAVE_LUA_H
	assert(ctx->wrk == ctx->srv->main_worker);
	if (!li_config_lua_load(&ctx->srv->LL, ctx->srv, ctx->wrk, val_fname->data.string->str, &a, TRUE, NULL)) {
		parse_error(ctx, error, "include_lua '%s' failed", val_fname->data.string->str);
		li_value_free(parameters);
		return FALSE;
	}

	/* include lua doesn't need to produce an action */
	if (NULL != a) {
		li_action_append_inplace(list, a);
		li_action_release(ctx->srv, a);
	}

	li_value_free(parameters);
	return TRUE;
#else
	li_value_free(parameters);
	return parse_error(ctx, error, "compiled without lua, include_lua not supported");
#endif
}

static gboolean p_include_shell(liAction *list, liConfigTokenizerContext *ctx, GError **error) {
	liValue *parameters = NULL;
	liValue *val_command;
	GString *command;
	gchar *cmd_stdout = NULL;
	liConfigTokenizerContext subctx;
	gboolean result;

	if (!p_parameter_values(&parameters, ctx, error)) return FALSE;

	val_command = li_value_get_single_argument(parameters);
	if (NULL == val_command || LI_VALUE_STRING != li_value_type(val_command)) {
		parse_error(ctx, error, "include_shell directive takes a string as parameter");
		goto error;
	}
	command = val_command->data.string;

	if (!ctx->master_config) {
		parse_error(ctx, error, "include_shell not allowed");
		goto error;
	}

	{
		gint status;
		GError *err = NULL;

		if (!g_spawn_command_line_sync(command->str, &cmd_stdout, NULL, &status, &err)) {
			parse_error(ctx, error, "include_shell '%s' failed: %s", command->str, err->message);
			g_error_free(err);
			goto error;
		}

		if (0 != status) {
			parse_error(ctx, error, "include_shell '%s' retured non-zero status: %i", command->str, status);
			goto error;
		}
	}

	g_string_prepend_len(command, CONST_STR_LEN("shell: "));

	tokenizer_init(ctx->srv, ctx->wrk, &subctx, command->str, cmd_stdout, strlen(cmd_stdout));

	subctx.master_config = ctx->master_config;

	scope_enter(&subctx);
	subctx.current_scope->parent = ctx->current_scope;
	result = p_actions(FALSE, list, &subctx, error);
	scope_leave(&subctx);
	g_free((gchar*) subctx.content);
	tokenizer_clear(&subctx);
	li_value_free(parameters);

	return result;

error:
	li_value_free(parameters);
	return FALSE;
}


static gboolean p_setup(GString *name, liConfigTokenizerContext *ctx, GError **error) {
	liValue *parameters = NULL;

	if (!p_parameter_values(&parameters, ctx, error)) return FALSE;

	if (g_str_equal(name->str, "__print")) {
		GString *s = li_value_to_string(parameters);
		DEBUG(ctx->srv, "config __print: %s", s->str);
		g_string_free(s, TRUE);
		li_value_free(parameters);
		return TRUE;
	} else if (!li_plugin_config_setup(ctx->srv, name->str, parameters)) {
		return parse_error(ctx, error, "setup '%s' failed", name->str);
	}

	return TRUE;
}

static gboolean p_setup_block(liConfigTokenizerContext *ctx, GError **error) {
	liConfigToken token;
	GString *name;
	gboolean result;

	NEXT(token);
	switch (token) {
	case TK_CURLY_CLOSE:
		return TRUE;
	case TK_NAME:
		name = g_string_new_len(GSTR_LEN(ctx->token_string));
		result = p_setup(name, ctx, error);
		g_string_free(name, TRUE);
		if (!result) return FALSE;
		break;
	case TK_INCLUDE:
	case TK_INCLUDE_LUA:
	case TK_INCLUDE_SHELL:
		return parse_error(ctx, error, "include not supported in setup mode");
	default:
		return parse_error(ctx, error, "expected name or block end }");
	}

	return p_setup_block(ctx, error);

error:
	return FALSE;
}

static gboolean p_action(liAction *list, GString *name, liConfigTokenizerContext *ctx, GError **error) {
	liValue *parameters = NULL;
	liAction *a = NULL;
	liValue *alias;

	alias = scope_peekvar(ctx, name);
	if (NULL != alias) {
		liConfigToken token;
		if (li_value_type(alias) != LI_VALUE_ACTION) {
			return parse_error(ctx, error, "'%s' is not an action variable", name->str);
		}
		NEXT(token);
		if (token != TK_SEMICOLON) {
			return parse_error(ctx, error, "action alias '%s' doesn't take any parameter, expected ';'", name->str);
		}
		li_action_append_inplace(list, alias->data.val_action.action);
		return TRUE;
error:
		return FALSE;
	}

	if (!p_parameter_values(&parameters, ctx, error)) return FALSE;

	if (g_str_equal(name->str, "__print")) {
		GString *s = li_value_to_string(parameters);
		DEBUG(ctx->srv, "config __print: %s", s->str);
		g_string_free(s, TRUE);
		li_value_free(parameters);
		return TRUE;
	} else if (NULL == (a = li_plugin_config_action(ctx->srv, ctx->wrk, name->str, parameters))) {
		return parse_error(ctx, error, "action '%s' failed", name->str);
	}

	li_action_append_inplace(list, a);
	li_action_release(ctx->srv, a);

	return TRUE;
}

/* parse actions until EOF (if !block) or '}' (if block, TK_CURLY_CLOSE) */
static gboolean p_actions(gboolean block, liAction *list, liConfigTokenizerContext *ctx, GError **error) {
	liConfigToken token;
	GString *name = NULL;
	gboolean result;

	NEXT(token);
	switch (token) {
	case TK_SETUP:
		NEXT(token);
		switch (token) {
		case TK_CURLY_OPEN:
			if (!p_setup_block(ctx, error)) return FALSE;
			break;
		case TK_NAME:
			name = g_string_new_len(GSTR_LEN(ctx->token_string));
			result = p_setup(name, ctx, error);
			g_string_free(name, TRUE); name = NULL;
			if (!result) return FALSE;
			break;
		case TK_INCLUDE:
		case TK_INCLUDE_LUA:
		case TK_INCLUDE_SHELL:
			return parse_error(ctx, error, "include not supported in setup mode");
		default:
			return parse_error(ctx, error, "expected name or block start '{'");
		}
		break;
	case TK_NAME:
		name = g_string_new_len(GSTR_LEN(ctx->token_string));
		NEXT(token);
		switch (token) {
		case TK_ASSIGN:
			if (!p_vardef(name, 0, ctx, error)) goto error;
			break;
		default:
			REMEMBER(token);
			if (!p_action(list, name, ctx, error)) goto error;
			break;
		}
		g_string_free(name, TRUE); name = NULL;
		break;
	case TK_LOCAL:
		NEXT(token);
		if (TK_NAME != token) return parse_error(ctx, error, "expected variable name after 'local'");
		name = g_string_new_len(GSTR_LEN(ctx->token_string));
		NEXT(token);
		if (TK_ASSIGN != token) {
			parse_error(ctx, error, "expected '=' after variable name");
			goto error;
		}
		if (!p_vardef(name, 1, ctx, error)) goto error;
		g_string_free(name, TRUE); name = NULL;
		break;
	case TK_GLOBAL:
		NEXT(token);
		if (TK_NAME != token) return parse_error(ctx, error, "expected variable name after 'global'");
		name = g_string_new_len(GSTR_LEN(ctx->token_string));
		NEXT(token);
		if (TK_ASSIGN != token) {
			parse_error(ctx, error, "expected '=' after variable name");
			goto error;
		}
		if (!p_vardef(name, 2, ctx, error)) goto error;
		g_string_free(name, TRUE); name = NULL;
		break;
	case TK_INCLUDE:
		if (!p_include(list, ctx, error)) goto error;
		break;
	case TK_INCLUDE_LUA:
		if (!p_include_lua(list, ctx, error)) goto error;
		break;
	case TK_INCLUDE_SHELL:
		if (!p_include_shell(list, ctx, error)) goto error;
		break;
	case TK_IF:
		if (!p_condition(list, ctx, error)) goto error;
		break;
	case TK_EOF:
		if (block) return parse_error(ctx, error, "unexpected end of file, expected name or '}'");
		return TRUE;
	case TK_CURLY_CLOSE:
		if (!block) return parse_error(ctx, error, "expected end of file instead of '}'");
		return TRUE;
	default:
		return parse_error(ctx, error, "unexpected token; expected action");
	}

	return p_actions(block, list, ctx, error);

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
			return parse_error(ctx, error, "casts not supported yet");
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
	case TK_CURLY_OPEN:
		{
			liAction *alist = li_action_new_list();
			scope_enter(ctx);
			if (!p_actions(TRUE, alist, ctx, error)) {
				scope_leave(ctx);
				li_action_release(ctx->srv, alist);
				return FALSE;
			}
			scope_leave(ctx);
			v = li_value_new_action(ctx->srv, alist);
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
		v = scope_getvar(ctx, ctx->token_string, error);
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


static gboolean p_vardef(GString *name, int normalLocalGlobal, liConfigTokenizerContext *ctx, GError **error) {
	liValue *value = NULL;
	gint key_value_nesting;
	liConfigToken token;

	if (!p_value(&key_value_nesting, &value, TK_ERROR, ctx, error)) return FALSE;
	NEXT(token);
	if (TK_SEMICOLON != token) {
		li_value_free(value);
		return parse_error(ctx, error, "expected ';'");
	}

	switch (normalLocalGlobal) {
	case 0:
		return scope_setvar(ctx, name, value, error);
	case 1:
		return scope_local_setvar(ctx, name, value, error);
	case 2:
		return scope_global_setvar(ctx, name, value, error);
	default:
		assert(normalLocalGlobal >= 0 && normalLocalGlobal <= 2);
	}

error:
	li_value_free(value);
	return FALSE;
}

static liAction* cond_walk(liServer *srv, liConditionTree *tree, liAction *positive, liAction *negative) {
	liAction *a = NULL;
	assert(NULL != tree);

	if (tree->negated) {
		liAction *tmp = negative; negative = positive; positive = tmp;
	}

	if (NULL != tree->condition) {
		assert(NULL == tree->left && NULL == tree->right);
		if (NULL == positive && NULL == negative) {
			li_condition_release(srv, tree->condition);
		} else {
			a = li_action_new_condition(tree->condition, positive, negative);
		}
	} else switch (tree->op) {
	case TK_AND:
		a = cond_walk(srv, tree->left, cond_walk(srv, tree->right, positive, negative), negative);
		break;
	case TK_OR:
		a = cond_walk(srv, tree->left, positive, cond_walk(srv, tree->right, positive, negative));
		break;
	default:
		assert(TK_AND == tree->op || TK_OR == tree->op);
	}

	g_slice_free(liConditionTree, tree);
	return a;
}

static gboolean p_condition_value(liConditionTree **tree, liConfigTokenizerContext *ctx, GError **error) {
	liConfigToken token;
	liCondLValue lval;
	liConditionLValue *lvalue = NULL;
	gint key_value_nesting = -1;
	liValue *rvalue = NULL;
	liCondition *cond = NULL;
	liCompOperator compop;

	NEXT(token);
	if (TK_NAME != token) return parse_error(ctx, error, "expected a condition variable");
	lval = li_cond_lvalue_from_string(GSTR_LEN(ctx->token_string));
	if (LI_COMP_UNKNOWN == lval) return parse_error(ctx, error, "unknown condition variable: %s", ctx->token_string->str);

	if (LI_COMP_REQUEST_HEADER == lval || LI_COMP_ENVIRONMENT == lval || LI_COMP_RESPONSE_HEADER == lval) {
		NEXT(token);
		if (TK_SQUARE_OPEN != token) return parse_error(ctx, error, "condition variable %s requires a key", li_cond_lvalue_to_string(lval));
		NEXT(token);
		if (TK_STRING != token) return parse_error(ctx, error, "expected a string as key to condition variable");
		lvalue = li_condition_lvalue_new(lval, g_string_new_len(GSTR_LEN(ctx->token_string)));
		NEXT(token);
		if (TK_SQUARE_CLOSE != token) {
			parse_error(ctx, error, "expected ']'");
			goto error;
		}
	} else {
		NEXT(token);
		if (TK_SQUARE_OPEN == token) return parse_error(ctx, error, "condition variable %s doesn't use a key", li_cond_lvalue_to_string(lval));
		REMEMBER(token);
		lvalue = li_condition_lvalue_new(lval, NULL);
	}

	NEXT(token);
	switch (token) {
	case TK_AND:
	case TK_OR:
	case TK_PARA_CLOSE:
	case TK_CURLY_OPEN:
		REMEMBER(token);
		cond = li_condition_new_bool(ctx->srv, lvalue, TRUE);
		goto boolcond;
	case TK_EQUAL:
		compop = LI_CONFIG_COND_EQ;
		break;
	case TK_GREATER:
		compop = LI_CONFIG_COND_GT;
		break;
	case TK_GREATER_EQUAL:
		compop = LI_CONFIG_COND_GE;
		break;
	case TK_LESS:
		compop = LI_CONFIG_COND_LT;
		break;
	case TK_LESS_EQUAL:
		compop = LI_CONFIG_COND_LE;
		break;
	case TK_MATCH:
		compop = LI_CONFIG_COND_MATCH;
		break;
	case TK_NOT_EQUAL:
		compop = LI_CONFIG_COND_NE;
		break;
	case TK_NOT_MATCH:
		compop = LI_CONFIG_COND_NOMATCH;
		break;
	case TK_NOT_PREFIX:
		compop = LI_CONFIG_COND_NOPREFIX;
		break;
	case TK_NOT_SUBNET:
		compop = LI_CONFIG_COND_NOTIP;
		break;
	case TK_NOT_SUFFIX:
		compop = LI_CONFIG_COND_NOSUFFIX;
		break;
	case TK_PREFIX:
		compop = LI_CONFIG_COND_NOSUFFIX;
		break;
	case TK_SUBNET:
		compop = LI_CONFIG_COND_IP;
		break;
	case TK_SUFFIX:
		compop = LI_CONFIG_COND_SUFFIX;
		break;
	default:
		parse_error(ctx, error, "expected comparison operator");
		goto error;
	}

	if (!p_value(&key_value_nesting, &rvalue, TK_OR, ctx, error)) goto error;

	switch (li_value_type(rvalue)) {
	case LI_VALUE_STRING:
		cond = li_condition_new_string(ctx->srv, compop, lvalue, li_value_extract_string(rvalue));
		break;
	case LI_VALUE_NUMBER:
		cond = li_condition_new_int(ctx->srv, compop, lvalue, rvalue->data.number);
		break;
	default:
		parse_error(ctx, error, "wrong value type, require string or integer");
		goto error;
	}
	li_value_free(rvalue);

boolcond:

	*tree = g_slice_new0(liConditionTree);
	(*tree)->negated = FALSE;
	(*tree)->condition = cond;

	return TRUE;

error:
	li_condition_lvalue_release(lvalue);
	li_value_free(rvalue);
	return FALSE;
}

static gboolean p_condition_expr(liConditionTree **tree, liConfigToken preOp, liConfigTokenizerContext *ctx, GError **error) {
	liConditionTree *leave = NULL;
	liConditionTree *result = NULL;
	liConfigToken token;
	liConfigToken op = TK_ERROR;

	for (;;) {
		gboolean negated = FALSE;
		NEXT(token);
		if (TK_NOT == token) {
			negated = TRUE;
			NEXT(token);
		}

		if (TK_PARA_OPEN == token) {
			if (!p_condition_expr(&leave, TK_ERROR, ctx, error)) goto error;
			NEXT(token);
			if (TK_PARA_CLOSE != token) {
				parse_error(ctx, error, "expected ')'");
				goto error;
			}
		} else {
			REMEMBER(token);
			if (!p_condition_value(&leave, ctx, error)) goto error;
		}
		if (negated) leave->negated = !leave->negated;

		if (result == NULL) {
			result = leave;
		} else {
			liConditionTree *tmp = g_slice_new0(liConditionTree);
			tmp->negated = FALSE;
			tmp->op = op;
			tmp->left = result;
			tmp->right = leave;
			result = tmp;
		}
		leave = NULL;

		NEXT(token);
		switch (token) {
		case TK_PARA_CLOSE:
		case TK_CURLY_OPEN:
			REMEMBER(token);
			*tree = result;
			return TRUE;
		case TK_AND:
		case TK_OR:
			if (operator_precedence(preOp, token)) {
				REMEMBER(token);
				*tree = result;
				return TRUE;
			}
			op = token;
			break;
		default:
			parse_error(ctx, error, "unexpected token in conditional");
			goto error;
		}
	}

error:
	if (NULL != result) cond_walk(ctx->srv, result, NULL, NULL);
	return FALSE;
}

static gboolean p_condition(liAction *list, liConfigTokenizerContext *ctx, GError **error) {
	liConditionTree *tree = NULL;
	liConfigToken token;
	liAction *positive = NULL, *negative = NULL;

	if (!p_condition_expr(&tree, TK_ERROR, ctx, error)) return FALSE;

	NEXT(token);
	if (TK_CURLY_OPEN != token) {
		parse_error(ctx, error, "expected '{'");
		goto error;
	}

	positive = li_action_new();
	if (!p_actions(TRUE, positive, ctx, error)) goto error;

	NEXT(token);
	if (TK_ELSE == token) {
		negative = li_action_new();
		NEXT(token);
		switch (token) {
		case TK_CURLY_OPEN:
			if (!p_actions(TRUE, negative, ctx, error)) goto error;
			break;
		case TK_IF:
			if (!p_condition(negative, ctx, error)) goto error;
			break;
		default:
			parse_error(ctx, error, "expected '{' or 'if' after 'else'");
			goto error;
		}
	} else {
		REMEMBER(token);
	}

	{
		liAction *a = cond_walk(ctx->srv, tree, positive, negative);
		li_action_append_inplace(list, a);
		li_action_release(ctx->srv, a);
	}

	return TRUE;

error:
	if (NULL != tree) cond_walk(ctx->srv, tree, NULL, NULL);
	li_action_release(ctx->srv, positive);
	li_action_release(ctx->srv, negative);
	return FALSE;
}

gboolean li_config_parse(liServer *srv, const gchar *config_path) {
	liConfigTokenizerContext ctx;
	GError *error = NULL;
	gboolean result;

	if (!tokenizer_init_file(srv, srv->main_worker, &ctx, config_path, &error)) {
		ERROR(srv, "%s", error->message);
		g_error_free(error);
		return FALSE;
	}

	ctx.master_config = TRUE;
	srv->mainaction = li_action_new();

	scope_enter(&ctx);
	result = p_actions(FALSE, srv->mainaction, &ctx, &error);
	scope_leave(&ctx);
	g_free((gchar*) ctx.content);
	tokenizer_clear(&ctx);

	if (!result) {
		ERROR(srv, "config error: %s", error->message);
		g_error_free(error);
		return FALSE;
	}

	{
		liAction *a_static = li_plugin_config_action(srv, srv->main_worker, "static", NULL);
		if (NULL == a_static) {
			ERROR(srv, "%s", "config error: couldn't create static action");
			return FALSE;
		}
		li_action_append_inplace(srv->mainaction, a_static);
		li_action_release(srv, a_static);
	}

	return TRUE;
}

liAction* li_config_parse_live(liWorker *wrk, const gchar *sourcename, const char *source, gsize sourcelen, GError **error) {
	liAction *list = li_action_new();
	liConfigTokenizerContext ctx;
	gboolean result;

	tokenizer_init(wrk->srv, wrk, &ctx, sourcename, source, sourcelen);
	ctx.master_config = FALSE;

	scope_enter(&ctx);
	result = p_actions(FALSE, list, &ctx, error);
	scope_leave(&ctx);
	tokenizer_clear(&ctx);

	if (!result) {
		li_action_release(wrk->srv, list);
		list = NULL;
	}

	return list;
}

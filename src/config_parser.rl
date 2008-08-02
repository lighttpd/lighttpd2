#include "base.h"
#include "condition.h"
#include "config_parser.h"

#if 1
	#define _printf(fmt, ...) g_print(fmt, __VA_ARGS__)
#else
	#define _printf(fmt, ...) /* */
#endif

/** config parser state machine **/

%%{
	## ragel stuff
	machine config_parser;

	variable p ctx->p;
	variable pe ctx->pe;
	variable eof ctx->eof;

	access ctx->;

	prepush {
		//_printf("current stacksize: %d, top: %d\n", ctx->stacksize, ctx->top);
		/* increase stacksize if necessary */
		if (ctx->stacksize == ctx->top)
		{
			/* increase stacksize by 8 */
			ctx->stack = g_realloc(ctx->stack, sizeof(int) * (ctx->stacksize + 8));
			ctx->stacksize += 8;
		}
	}

	## actions
	action mark { ctx->mark = fpc; }

	# basic types
	action boolean {
		option *o;

		o = option_new_bool(*ctx->mark == 't' ? TRUE : FALSE);
		g_queue_push_head(ctx->option_stack, o);

		_printf("got boolean %s in line %zd\n", *ctx->mark == 't' ? "true" : "false", ctx->line);
	}

	action integer {
		option *o;
		guint i = 0;

		for (gchar *c = ctx->mark; c < fpc; c++)
			i = i * 10 + *c - 48;

		o = option_new_int(i);
		/* push option onto stack */
		g_queue_push_head(ctx->option_stack, o);

		_printf("got integer %d in line %zd\n", i, ctx->line);
	}

	action integer_suffix {
	}

	action string {
		option *o;
		GString *str;

		str = g_string_new_len(ctx->mark+1, fpc - ctx->mark - 2);
		o = option_new_string(str);
		g_queue_push_head(ctx->option_stack, o);

		_printf("got string %s", "");
		for (gchar *c = ctx->mark + 1; c < fpc - 1; c++) _printf("%c", *c);
		_printf(" in line %zd\n", ctx->line);
	}

	# advanced types
	action list_start {
		option *o;

		/* create new list option and put it on stack, list entries are put in it by getting the previous option from the stack */
		o = option_new_list();
		g_queue_push_head(ctx->option_stack, o);

		fcall list_scanner;
	}

	action list_push {
		option *o, *l;

		/* pop current option form stack and append it to the new top of the stack option (the list) */
		o = g_queue_pop_head(ctx->option_stack);

		l = g_queue_peek_head(ctx->option_stack);
		assert(l->type == OPTION_LIST);

		g_array_append_val(l->value.opt_list, o);

		_printf("list_push%s\n", "");
	}

	action list_end {
		fret;
	}

	action hash_start {
		option *o;

		/* create new hash option and put it on stack, if a key-value pair is encountered, get it by walking 2 steps back the stack */
		o = option_new_hash();
		g_queue_push_head(ctx->option_stack, o);

		fcall hash_scanner;
	}

	action hash_push {
		option *k, *v, *h; /* key value hashtable */
		GString *str;

		v = g_queue_pop_head(ctx->option_stack);
		k = g_queue_pop_head(ctx->option_stack);
		h = g_queue_peek_head(ctx->option_stack);

		/* duplicate key so option can be free'd */
		str = g_string_new_len(k->value.opt_string->str, k->value.opt_string->len);

		g_hash_table_insert(h->value.opt_hash, str, v);
		option_free(k);

		_printf("%s\n", "hash_push");
	}

	action hash_end {
		fret;
	}

	action block_start {
		fcall block_scanner;
	}

	action block_end {
		fret;
	}

	action keyvalue_start {
		fcall keyvalue_scanner;
	}

	action keyvalue_end {
		option *k, *v, *l;
		/* we have a key and a value on the stack; convert them to a list with 2 elements */

		v = g_queue_pop_head(ctx->option_stack);
		k = g_queue_pop_head(ctx->option_stack);

		assert(k->type == OPTION_STRING);

		l = option_new_list();

		g_array_append_val(l->value.opt_list, k);
		g_array_append_val(l->value.opt_list, v);

		/* push list on the stack */
		g_queue_push_head(ctx->option_stack, l);

		fret;
	}

	action value {
		/*
		_printf("got value %s", "");
		for (gchar *c = ctx->mark; c < fpc; c++) _printf("%c", *c);
		_printf(" in line %zd\n", ctx->line);
		*/
	}

	action varname {
		/* varname, push it as string option onto the stack */
		option *o;
		GString *str;

		str = g_string_new_len(ctx->mark, fpc - ctx->mark);
		o = option_new_string(str);
		g_queue_push_head(ctx->option_stack, o);
	}

	# statements
	action assignment {
		option *val, *name;

		/* top of the stack is the value, then the varname as string option */
		val = g_queue_pop_head(ctx->option_stack);
		name = g_queue_pop_head(ctx->option_stack);

		assert(name->type == OPTION_STRING);

		g_print("got assignment: %s = %s; in line %zd\n", name->value.opt_string->str, option_type_string(val->type), ctx->line);
	}

	action function {
		/* similar to assignment */
		option *val, *name;

		/* top of the stack is the value, then the varname as string option */
		val = g_queue_pop_head(ctx->option_stack);
		name = g_queue_pop_head(ctx->option_stack);

		assert(name->type == OPTION_STRING);

		if (g_str_equal(name->value.opt_string->str, "include")) {
			if (val->type != OPTION_STRING) {
				/* TODO */
			}
		}
		else if (g_str_equal(name->value.opt_string->str, "include_shell")) {
			if (val->type != OPTION_STRING) {
				/* TODO */
			}
		}
		else {
			/* TODO */
		}

		g_print("got function: %s %s; in line %zd\n", name->value.opt_string->str, option_type_string(val->type), ctx->line);
	}

	action condition {
		/* stack: value, varname */
		option *v, *n;

		v = g_queue_pop_head(ctx->option_stack);
		n = g_queue_pop_head(ctx->option_stack);
		assert(n->type == OPTION_STRING);

		g_print("got condition: %s %s %s in line %zd\n", n->value.opt_string->str, option_type_string(v->type), comp_op_to_string(ctx->op), ctx->line);
	}

	action action_block {
		option *o;

		o = g_queue_pop_head(ctx->option_stack);
		assert(o->type == OPTION_STRING);

		g_print("action block %s in line %zd\n", o->value.opt_string->str, ctx->line);
	}


	## definitions

	# basic types
	boolean = ( 'true' | 'false' ) %boolean;
	integer = ( 0 | ( [1-9] [0-9]* ) ) %integer;
	integer_suffix_bytes = ( 'b' | 'kb' | 'mb' | 'gb' | 'tb' | 'pb' );
	integer_suffix_seconds = ( 's' | 'm' | 'h' | 'd' );
	string = ( '"' (any-'"')* '"' ) %string;

	# misc stuff
	line_sane = ( '\n' ) >{ ctx->line++; };
	line_weird = ( '\r' ) >{ ctx->line++; };
	line_insane = ( '\r\n' ) >{ ctx->line--; };
	line = ( line_sane | line_weird | line_insane );

	ws = ( '\t' | ' ' );
	noise = ( ws | line );

	comment = ( '#' (any - line)* line );
	block = ( '{' >block_start );

	# advanced types
	varname = ( alpha ( alnum | [._\-] )* ) >mark %varname;
	actionref = ( varname );
	list = ( '(' >list_start );
	hash = ( '[' >hash_start );
	keyvalue = ( string ws* '=>' ws* >keyvalue_start );
	value = ( boolean | integer | string | list | hash | keyvalue | actionref ) >mark %value;
	hash_elem = ( string >mark noise* ':' noise* value );

	operator = ( '==' | '!=' | '<' | '<=' | '>' | '>=' | '=~' | '!~' );

	# statements
	assignment = ( varname ws* '=' ws* value ';' ) %assignment;
	function = ( varname ws+ value ';' ) %function;
	condition = ( varname ws* operator ws* value noise* block ) %condition;
	action_block = ( varname ws* block ) %action_block;

	statement = ( assignment | function | condition | action_block );

	# scanner
	keyvalue_scanner := ( value any >{fpc--;} >keyvalue_end );
	block_scanner := ( (noise | comment | statement)* '}' >block_end );
	list_scanner := ( noise* (value %list_push (noise* ',' noise* value %list_push)*)? noise* ')' >list_end );
	hash_scanner := ( noise* (hash_elem %hash_push (noise* ',' noise* hash_elem %hash_push)*)? noise* ']' >hash_end );

	main := (noise | comment | statement)* '\00';
}%%

%% write data;

config_parser_context_t *config_parser_context_new(GList *ctx_stack) {
	config_parser_context_t *ctx;

	ctx = g_slice_new0(config_parser_context_t);

	ctx->line = 1;

	/* allocate stack of 8 items. sufficient for most configs, will grow when needed */
	ctx->stack = (int*) g_malloc(sizeof(int) * 8);
	ctx->stacksize = 8;

	if (ctx_stack != NULL) {
		/* inherit old stacks */
		ctx->action_list_stack = ((config_parser_context_t*) ctx_stack->data)->action_list_stack;
		ctx->option_stack = ((config_parser_context_t*) ctx_stack->data)->option_stack;
	}
	else {
		ctx->action_list_stack = g_queue_new();
		ctx->option_stack = g_queue_new();
	}

	return ctx;
}

void config_parser_context_free(config_parser_context_t *ctx, gboolean free_queues)
{
	g_free(ctx->stack);

	if (free_queues) {
		g_assert_cmpuint(ctx->action_list_stack->length, ==, 0);
		g_assert_cmpuint(ctx->option_stack->length, ==, 0);
		g_queue_free(ctx->action_list_stack);
		g_queue_free(ctx->option_stack);
	}

	g_slice_free(config_parser_context_t, ctx);
}

gboolean config_parser_file(server *srv, GList *ctx_stack, const gchar *path) {
	config_parser_context_t *ctx;
	gboolean res;
	GError *err = NULL;

	ctx = config_parser_context_new(ctx_stack);
	ctx->filename = (gchar*) path;

	if (!g_file_get_contents(path, &ctx->ptr, &ctx->len, &err))
	{
		/* could not read file */
		log_warning(srv, NULL, "could not read config file \"%s\". reason: \"%s\" (%d)", path, err->message, err->code);
		config_parser_context_free(ctx, FALSE);
		g_error_free(err);
		return FALSE;
	}

	/* push on stack */
	ctx_stack = g_list_prepend(ctx_stack, ctx);

	res = config_parser_buffer(srv, ctx_stack);

	if (!res)
		log_warning(srv, NULL, "config parsing failed in line %zd of %s", ctx->line, ctx->filename);

	/* pop from stack */
	ctx_stack = g_list_delete_link(ctx_stack, ctx_stack);

	/* have to free the buffer on our own */
	g_free(ctx->ptr);
	config_parser_context_free(ctx, FALSE);

	return res;
}

gboolean config_parser_shell(server *srv, GList *ctx_stack, const gchar *command)
{
	gboolean res;
	gchar* _stdout;
	gchar* _stderr;
	gint status;
	config_parser_context_t *ctx;
	GError *err = NULL;

	ctx = config_parser_context_new(ctx_stack);
	ctx->filename = (gchar*) command;

	if (!g_spawn_command_line_sync(command, &_stdout, &_stderr, &status, &err))
	{
		log_warning(srv, NULL, "error launching shell command \"%s\": %s (%d)", command, err->message, err->code);
		config_parser_context_free(ctx, FALSE);
		g_error_free(err);
		return FALSE;
	}

	if (status != 0)
	{
		log_warning(srv, NULL, "shell command \"%s\" exited with status %d", command, status);
		log_debug(srv, NULL, "stdout:\n-----\n%s\n-----\nstderr:\n-----\n%s\n-----", _stdout, _stderr);
		g_free(_stdout);
		g_free(_stderr);
		config_parser_context_free(ctx, FALSE);
		return FALSE;
	}

	ctx->len = strlen(_stdout);
	ctx->ptr = _stdout;

	log_debug(srv, NULL, "included shell output from \"%s\" (%zu bytes)", command, ctx->len, _stdout);

	/* push on stack */
	ctx_stack = g_list_prepend(ctx_stack, ctx);
	/* parse buffer */
	res = config_parser_buffer(srv, ctx_stack);
	/* pop from stack */
	ctx_stack = g_list_delete_link(ctx_stack, ctx_stack);

	g_free(_stdout);
	g_free(_stderr);
	config_parser_context_free(ctx, FALSE);

	return res;
}

gboolean config_parser_buffer(server *srv, GList *ctx_stack)
{
	config_parser_context_t *ctx;

	/* get top of stack */
	ctx = (config_parser_context_t*) ctx_stack->data;

	ctx->p = ctx->ptr;
	ctx->pe = ctx->ptr + ctx->len + 1; /* marks the end of the data to scan (+1 because of trailing \0 char) */

	%% write init;

	%% write exec;

	if (ctx->cs == config_parser_error || ctx->cs == config_parser_first_final)
	{
		/* parse error */
		log_warning(srv, NULL, "parse error in line %zd of \"%s\" at character %c (0x%.2x)", ctx->line, ctx->filename, *ctx->p, *ctx->p);
		return FALSE;
	}

	return TRUE;
}

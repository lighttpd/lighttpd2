#include "base.h"
#include "condition.h"
#include "config_parser.h"

#if 0
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
		option *o;
		GString *str;

		o = g_queue_peek_head(ctx->option_stack);

		str = g_string_new_len(ctx->mark, fpc - ctx->mark);

		     if (g_str_equal(str->str, "kbyte")) o->value.opt_int *= 1024;
		else if (g_str_equal(str->str, "mbyte")) o->value.opt_int *= 1024 * 1024;
		else if (g_str_equal(str->str, "gbyte")) o->value.opt_int *= 1024 * 1024 * 1024;

		else if (g_str_equal(str->str, "kbit")) o->value.opt_int *= 1024;
		else if (g_str_equal(str->str, "mbit")) o->value.opt_int *= 1024 * 1024;
		else if (g_str_equal(str->str, "gbit")) o->value.opt_int *= 1024 * 1024 * 1024;

		else if (g_str_equal(str->str, "min")) o->value.opt_int *= 60;
		else if (g_str_equal(str->str, "hours")) o->value.opt_int *= 60 * 60;
		else if (g_str_equal(str->str, "days")) o->value.opt_int *= 60 * 60 * 24;

		g_string_free(str, TRUE);

		_printf("got int with suffix: %d\n", o->value.opt_int);

		/* make sure there was no overflow that led to negative numbers */
		if (o->value.opt_int < 0) {
			log_warning(srv, NULL, "integer value overflowed in line %zd of %s\n", ctx->line, ctx->filename);
			return FALSE;
		}
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

		_printf("list_push %s\n", option_type_string(o->type));
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
		fpc--;

		fcall value_scanner;
	}

	action keyvalue_end {
		option *k, *v, *l;
		/* we have a key and a value on the stack; convert them to a list with 2 elements */

		v = g_queue_pop_head(ctx->option_stack);
		k = g_queue_pop_head(ctx->option_stack);

		l = option_new_list();

		g_array_append_val(l->value.opt_list, k);
		g_array_append_val(l->value.opt_list, v);

		/* push list on the stack */
		g_queue_push_head(ctx->option_stack, l);

		fpc--;

		fret;
	}

	action value {
		/*
		_printf("got value %s", "");
		for (gchar *c = ctx->mark; c < fpc; c++) _printf("%c", *c);
		_printf(" in line %zd\n", ctx->line);
		*/
	}

	action value_statement_op {
		ctx->value_op = *fpc;
	}

	action value_statement {
		/* value (+|-|*|/) value */
		/* compute new value out of the two */
		option *l, *r, *o;
		gboolean free_l, free_r;

		free_l = free_r = TRUE;

		r = g_queue_pop_head(ctx->option_stack);
		l = g_queue_pop_head(ctx->option_stack);
		o = NULL;


		if (l->type == OPTION_INT && r->type == OPTION_INT) {
			switch (ctx->value_op) {
				case '+': o = option_new_int(l->value.opt_int + r->value.opt_int); break;
				case '-': o = option_new_int(l->value.opt_int - r->value.opt_int); break;
				case '*': o = option_new_int(l->value.opt_int * r->value.opt_int); break;
				case '/': o = option_new_int(l->value.opt_int / r->value.opt_int); break;
			}
		}
		else if (l->type == OPTION_STRING) {
			o = l;
			free_l = FALSE;

			if (r->type == OPTION_STRING && ctx->value_op == '+') {
				/* str + str */
				o->value.opt_string = g_string_append_len(o->value.opt_string, r->value.opt_string->str, r->value.opt_string->len);
			}
			else if (r->type == OPTION_INT && ctx->value_op == '*') {
				/* str * int */
				if (r->value.opt_int < 0) {
					log_warning(srv, NULL, "string multiplication with negative number (%d)?", r->value.opt_int);
					return FALSE;
				}
				else if (r->value.opt_int == 0) {
					o->value.opt_string = g_string_truncate(o->value.opt_string, 0);
				}
				else {
					GString *str;
					str = g_string_new_len(l->value.opt_string->str, l->value.opt_string->len);
					for (gint i = 1; i < r->value.opt_int; i++)
						o->value.opt_string = g_string_append_len(o->value.opt_string, str->str, str->len);
					g_string_free(str, TRUE);
				}
			}
		}
		else if (l->type == OPTION_LIST) {
			if (ctx->value_op == '+') {
				/* append r to the end of l */
				free_l = FALSE; /* use l as the new o */
				free_r = FALSE; /* r gets appended to o */
				o = l;

				g_array_append_val(o->value.opt_list, r);
			}
			else if (ctx->value_op == '*') {
				/* merge l and r */
				GArray *a;
				free_l = FALSE;
				o = l;

				a = g_array_sized_new(FALSE, FALSE, sizeof(option*), r->value.opt_list->len);
				a = g_array_append_vals(a, r->value.opt_list->data, r->value.opt_list->len); /* copy old list from r to a */
				o->value.opt_list = g_array_append_vals(o->value.opt_list, a->data, a->len); /* append data from a to o */
				g_array_free(a, FALSE); /* free a but not the data because it is now in o */
			}
		}
		else if (l->type == OPTION_HASH && r->type == OPTION_HASH && ctx->value_op == '+') {
			/* merge hashtables */
			GHashTableIter iter;
			gpointer key, value;
			free_l = FALSE; /* keep l, it's the new o */
			o = l;

			g_hash_table_iter_init(&iter, r->value.opt_hash);
			while (g_hash_table_iter_next(&iter, &key, &value)) {
				g_hash_table_insert(o->value.opt_hash, key, value);
				g_hash_table_iter_steal(&iter); /* steal key->value so it doesn't get deleted when destroying r */
			}
		}

		if (o == NULL) {
			log_warning(srv, NULL, "erronous value statement: %s %c %s in line %zd\n", option_type_string(l->type), ctx->value_op, option_type_string(r->type), ctx->line);
			return FALSE;
		}

		_printf("value statement: %s %c %s => %s in line %zd\n", option_type_string(l->type), ctx->value_op, option_type_string(r->type), option_type_string(o->type), ctx->line);

		if (free_l)
			option_free(l);
		if (free_r)
			option_free(r);

		g_queue_push_head(ctx->option_stack, o);
	}

	action varname {
		/* varname, push it as string option onto the stack */
		option *o;
		GString *str;

		str = g_string_new_len(ctx->mark, fpc - ctx->mark);
		o = option_new_string(str);
		g_queue_push_head(ctx->option_stack, o);
	}

	action actionref {
		/* varname is on the stack */
		option *o, *r;

		o = g_queue_pop_head(ctx->option_stack);

		/* action refs starting with "var." are user defined variables */
		if (g_str_has_prefix(o->value.opt_string->str, "var.")) {
			/* look up var in hashtable, push option onto stack */
			r = g_hash_table_lookup(ctx->uservars, o->value.opt_string);

			if (r == NULL) {
				log_warning(srv, NULL, "unknown variable '%s'", o->value.opt_string->str);
				return FALSE;
			}
		}
		else {
			/* real action, lookup hashtable and create new action option */
			action *a;
			a = g_hash_table_lookup(ctx->action_blocks, o->value.opt_string);

			if (a == NULL) {
				log_warning(srv, NULL, "unknown action block referenced: %s", o->value.opt_string->str);
				return FALSE;
			}

			r = option_new_action(srv, a);
		}

		g_queue_push_head(ctx->option_stack, r);
		option_free(o);
	}

	action operator {
		if ((fpc - ctx->mark) == 1) {
			switch (*ctx->mark) {
				case '<': ctx->op = CONFIG_COND_LT; break;
				case '>': ctx->op = CONFIG_COND_GT; break;
			}
		}
		else {
			     if (*ctx->mark == '>' && *(ctx->mark+1) == '=') ctx->op = CONFIG_COND_GE;
			else if (*ctx->mark == '<' && *(ctx->mark+1) == '=') ctx->op = CONFIG_COND_LE;
			else if (*ctx->mark == '=' && *(ctx->mark+1) == '=') ctx->op = CONFIG_COND_EQ;
			else if (*ctx->mark == '!' && *(ctx->mark+1) == '=') ctx->op = CONFIG_COND_NE;
			else if (*ctx->mark == '=' && *(ctx->mark+1) == '^') ctx->op = CONFIG_COND_PREFIX;
			else if (*ctx->mark == '!' && *(ctx->mark+1) == '^') ctx->op = CONFIG_COND_NOPREFIX;
			else if (*ctx->mark == '=' && *(ctx->mark+1) == '$') ctx->op = CONFIG_COND_SUFFIX;
			else if (*ctx->mark == '!' && *(ctx->mark+1) == '$') ctx->op = CONFIG_COND_NOSUFFIX;
			else if (*ctx->mark == '=' && *(ctx->mark+1) == '~') ctx->op = CONFIG_COND_MATCH;
			else if (*ctx->mark == '!' && *(ctx->mark+1) == '~') ctx->op = CONFIG_COND_NOMATCH;
		}
	}

	# statements
	action assignment {
		option *val, *name;
		action *a, *al;

		/* top of the stack is the value, then the varname as string option */
		val = g_queue_pop_head(ctx->option_stack);
		name = g_queue_pop_head(ctx->option_stack);

		assert(name->type == OPTION_STRING);

		_printf("got assignment: %s = %s; in line %zd\n", name->value.opt_string->str, option_type_string(val->type), ctx->line);

		if (ctx->in_setup_block) {
			/* in setup { } block => set default values for options */
			/* todo name */
		}
		else if (g_str_has_prefix(name->value.opt_string->str, "var.")) {
			/* assignment vor user defined variable, insert into hashtable */
			g_hash_table_insert(ctx->uservars, name->value.opt_string, val);
		}
		else {
			/* normal assignment */
			a = option_action(srv, name->value.opt_string->str, val);

			if (a == NULL)
				return FALSE;

			al = g_queue_peek_head(ctx->action_list_stack);
			g_array_append_val(al->value.list, a);
			option_free(name);
		}
	}

	action function_noparam {
		option *name;
		action *a, *al;

		name = g_queue_pop_head(ctx->option_stack);

		assert(name->type == OPTION_STRING);

		_printf("got function: %s; in line %zd\n", name->value.opt_string->str, ctx->line);

		if (g_str_equal(name->value.opt_string->str, "break")) {
		}
		else if (g_str_equal(name->value.opt_string->str, "__halt")) {
		}
		else {
			if (ctx->in_setup_block) {
				/* we are in the setup { } block, call setups and don't append to action list */
				if (!call_setup(srv, name->value.opt_string->str, NULL)) {
					return FALSE;
				}
			}
			else {
				al = g_queue_peek_head(ctx->action_list_stack);
				a = create_action(srv, name->value.opt_string->str, NULL);

				if (a == NULL)
					return FALSE;

				g_array_append_val(al->value.list, a);
			}
		}
	}

	action function_param {
		/* similar to assignment */
		option *val, *name;
		action *a, *al;

		/* top of the stack is the value, then the varname as string option */
		val = g_queue_pop_head(ctx->option_stack);
		name = g_queue_pop_head(ctx->option_stack);

		assert(name->type == OPTION_STRING);

		_printf("got function: %s %s; in line %zd\n", name->value.opt_string->str, option_type_string(val->type), ctx->line);

		if (g_str_equal(name->value.opt_string->str, "include")) {
			if (val->type != OPTION_STRING) {
				log_warning(srv, NULL, "include directive takes a string as parameter, %s given", option_type_string(val->type));
				return FALSE;
			}

			if (!config_parser_file(srv, ctx_stack, val->value.opt_string->str))
				return FALSE;

			option_free(val);
		}
		else if (g_str_equal(name->value.opt_string->str, "include_shell")) {
			if (val->type != OPTION_STRING) {
				log_warning(srv, NULL, "include_shell directive takes a string as parameter, %s given", option_type_string(val->type));
				return FALSE;
			}

			if (!config_parser_shell(srv, ctx_stack, val->value.opt_string->str))
				return FALSE;

			option_free(val);
		}
		else {
			/* TODO */
			if (ctx->in_setup_block) {
				/* we are in the setup { } block, call setups and don't append to action list */
				if (!call_setup(srv, name->value.opt_string->str, val)) {
					return FALSE;
				}
			}
			else {
				al = g_queue_peek_head(ctx->action_list_stack);
				a = create_action(srv, name->value.opt_string->str, val);

				if (a == NULL)
					return FALSE;

				g_array_append_val(al->value.list, a);
			}
		}

		option_free(name);
	}

	action condition_start {
		/* stack: value, varname OR value, key, varname */
		option *v, *n, *k;
		gchar *str;
		condition *cond;
		condition_lvalue *lvalue;

		v = g_queue_pop_head(ctx->option_stack);
		if (ctx->condition_with_key)
			k = g_queue_pop_head(ctx->option_stack);
		else
			k = NULL;
		n = g_queue_pop_head(ctx->option_stack);

		assert(n->type == OPTION_STRING);

		_printf("got condition: %s:%s %s %s in line %zd\n", n->value.opt_string->str, ctx->condition_with_key ? k->value.opt_string->str : "", comp_op_to_string(ctx->op), option_type_string(v->type), ctx->line);

		/* create condition lvalue */
		str = n->value.opt_string->str;

		if (g_str_has_prefix(str, "req")) {
			str += 3;
			if (g_str_has_prefix(str, "."))
				str++;
			else if (g_str_has_prefix(str, "uest."))
				str += 5;
			else {
				log_warning(srv, NULL, "unkown lvalue for condition: %s", n->value.opt_string->str);
				return FALSE;
			}

			if (g_str_equal(str, "host"))
				lvalue = condition_lvalue_new(COMP_REQUEST_HOST, NULL);
			else if (g_str_equal(str, "path"))
				lvalue = condition_lvalue_new(COMP_REQUEST_PATH, NULL);
			else if (g_str_equal(str, "query"))
				lvalue = condition_lvalue_new(COMP_REQUEST_QUERY_STRING, NULL);
			else if (g_str_equal(str, "method"))
				lvalue = condition_lvalue_new(COMP_REQUEST_METHOD, NULL);
			else if (g_str_equal(str, "scheme"))
				lvalue = condition_lvalue_new(COMP_REQUEST_SCHEME, NULL);
			else if (g_str_equal(str, "header")) {
				if (k == NULL) {
					log_warning(srv, NULL, "header conditional needs a key", "");
					return FALSE;
				}
				lvalue = condition_lvalue_new(COMP_REQUEST_HEADER, k->value.opt_string);
			}
			else {
				log_warning(srv, NULL, "unkown lvalue for condition: %s", n->value.opt_string->str);
				return FALSE;
			}
		}
		else if (g_str_has_prefix(str, "phys")) {
			str += 3;
			if (g_str_has_prefix(str, "."))
				str++;
			else if (g_str_has_prefix(str, "ical."))
				str += 5;
			else {
				log_warning(srv, NULL, "unkown lvalue for condition: %s", n->value.opt_string->str);
				return FALSE;
			}

			if (g_str_equal(str, "path"))
				lvalue = condition_lvalue_new(COMP_PHYSICAL_PATH, NULL);
			else if (g_str_equal(str, "exists"))
				lvalue = condition_lvalue_new(COMP_PHYSICAL_PATH_EXISTS, NULL);
			else if (g_str_equal(str, "size"))
				lvalue = condition_lvalue_new(COMP_PHYSICAL_SIZE, NULL);
			else {
				log_warning(srv, NULL, "unkown lvalue for condition: %s", n->value.opt_string->str);
				return FALSE;
			}
		}
		else {
			log_warning(srv, NULL, "unkown lvalue for condition: %s", n->value.opt_string->str);
			return FALSE;
		}

		if (v->type == OPTION_STRING) {
			cond = condition_new_string(srv, ctx->op, lvalue, v->value.opt_string);
		}
		else if (v->type == OPTION_INT)
			cond = condition_new_int(srv, ctx->op, lvalue, (gint64) v->value.opt_int);
		else {
			cond = NULL;
		}


		if (cond == NULL) {
			log_warning(srv, NULL, "could not create condition", "");
			return FALSE;
		}

		g_queue_push_head(ctx->condition_stack, cond);

		g_queue_push_head(ctx->action_list_stack, action_new_list());

		/* TODO: free stuff */
		ctx->condition_with_key = FALSE;
	}

	action condition_end {
		condition *cond;
		action *a, *al;

		cond = g_queue_pop_head(ctx->condition_stack);
		al = g_queue_pop_head(ctx->action_list_stack);
		a = action_new_condition(cond, al);
		al = g_queue_peek_head(ctx->action_list_stack);
		g_array_append_val(al->value.list, a);
	}

	action condition_key {
		ctx->condition_with_key = TRUE;
	}

	action action_block_start {
		option *o;
		action *al;

		o = g_queue_pop_head(ctx->option_stack);
		assert(o->type == OPTION_STRING);

		if (ctx->in_setup_block) {
			/* no block inside the setup block allowed */
			assert(NULL); /* TODO */
		}

		if (g_str_equal(o->value.opt_string->str, "setup")) {
			_printf("entered setup block in line %zd\n", ctx->line);
			ctx->in_setup_block = TRUE;
		}
		else {
			GString *str;

			_printf("action block %s in line %zd\n", o->value.opt_string->str, ctx->line);

			/* create new action list and put it on the stack */
			al = action_new_list();
			g_queue_push_head(ctx->action_list_stack, al);
			/* insert into hashtable for later lookups */
			str = g_string_new_len(o->value.opt_string->str, o->value.opt_string->len);
			g_hash_table_insert(ctx->action_blocks, str, al);
		}

		option_free(o);
	}

	action action_block_end {
		if (ctx->in_setup_block) {
			ctx->in_setup_block = FALSE;
		}
		else {
			/* pop action list stack */
			g_queue_pop_head(ctx->action_list_stack);
		}
	}


	## definitions

	# misc stuff
	line_sane = ( '\n' ) >{ ctx->line++; };
	line_weird = ( '\r' ) >{ ctx->line++; };
	line_insane = ( '\r\n' ) >{ ctx->line--; };
	line = ( line_sane | line_weird | line_insane );

	ws = ( '\t' | ' ' );
	comment = ( '#' (any - line)* line );
	noise = ( ws | line | comment );

	block = ( '{' >block_start );

	# basic types
	boolean = ( 'true' | 'false' ) %boolean;
	integer_suffix_bytes = ( 'byte' | 'kbyte' | 'mbyte' | 'gbyte' | 'tbyte' | 'pbyte' );
	integer_suffix_bits = ( 'bit' | 'kbit' | 'mbit' | 'gbit' );
	integer_suffix_seconds = ( 'sec' | 'min' | 'hours' | 'days' );
	integer_suffix = ( integer_suffix_bytes | integer_suffix_bits | integer_suffix_seconds ) >mark %integer_suffix;
	integer = ( 0 | ( [1-9] [0-9]* ) %integer (ws? integer_suffix)? );
	string = ( '"' (any-'"')* '"' ) %string;

	# advanced types
	varname = ( (alpha ( alnum | [._] )*) - boolean ) >mark %varname;
	actionref = ( varname ) %actionref;
	list = ( '(' >list_start );
	hash = ( '[' >hash_start );
	keyvalue = ( (string | integer) ws* '=>' ws* (any - ws) >keyvalue_start );
	value = ( boolean | integer | string | list | hash | keyvalue | actionref ) >mark %value;
	value_statement = ( value (ws* ('+'|'-'|'*'|'/') >value_statement_op ws* value %value_statement)? );
	hash_elem = ( string >mark noise* ':' noise* value );

	operator = ( '==' | '!=' | '=^' | '!^' | '=$' | '!$' | '<' | '<=' | '>' | '>=' | '=~' | '!~' ) >mark %operator;

	# statements
	assignment = ( varname ws* '=' ws* value_statement ';' ) %assignment;
	function_noparam = ( varname ';' ) %function_noparam;
	function_param = ( varname ws+ value_statement ';') %function_param;
	function = ( function_noparam | function_param );
	condition = ( varname ('[' string >mark ']' %condition_key)? ws* operator ws* value_statement noise* block >condition_start ) %condition_end;
	action_block = ( varname noise* block >action_block_start ) %action_block_end;

	statement = ( assignment | function | condition | action_block );

	# scanner
	value_scanner := ( value (any - value - ws) >keyvalue_end );
	block_scanner := ( (noise | comment | statement)* '}' >block_end );
	list_scanner := ( noise* (value %list_push (noise* ',' noise* value %list_push)*)? noise* ')' >list_end );
	hash_scanner := ( noise* (hash_elem %hash_push (noise* ',' noise* hash_elem %hash_push)*)? noise* ']' >hash_end );

	main := (noise | comment | statement)* '\00';
}%%

%% write data;


GList *config_parser_init(server* srv) {
	config_parser_context_t *ctx = config_parser_context_new(srv, NULL);

	srv->mainaction = action_new_list();
	g_queue_push_head(ctx->action_list_stack, srv->mainaction);

	return g_list_append(NULL, ctx);
}

void config_parser_finish(server *srv, GList *ctx_stack) {
	config_parser_context_t *ctx;
	GHashTableIter iter;
	gpointer key, value;

	_printf("ctx_stack size: %u\n", g_list_length(ctx_stack));

	/* clear all contexts from the stack */
	while ((ctx = g_list_nth_data(ctx_stack, 1)) != NULL) {
		config_parser_context_free(srv, ctx, FALSE);
	}

	ctx = (config_parser_context_t*) ctx_stack->data;

	g_hash_table_iter_init(&iter, ctx->action_blocks);

	while (g_hash_table_iter_next(&iter, &key, &value)) {
		g_hash_table_iter_steal(&iter);
		g_string_free(key, TRUE);
	}

	g_hash_table_destroy(ctx->action_blocks);



	g_hash_table_iter_init(&iter, ctx->uservars);

	while (g_hash_table_iter_next(&iter, &key, &value)) {
		g_hash_table_iter_steal(&iter);
		g_string_free(key, TRUE);
	}

	g_hash_table_destroy(ctx->uservars);



	config_parser_context_free(srv, ctx, TRUE);

	g_list_free(ctx_stack);
}

config_parser_context_t *config_parser_context_new(server *srv, GList *ctx_stack) {
	config_parser_context_t *ctx;

	UNUSED(srv);

	ctx = g_slice_new0(config_parser_context_t);

	ctx->line = 1;

	/* allocate stack of 8 items. sufficient for most configs, will grow when needed */
	ctx->stack = (int*) g_malloc(sizeof(int) * 8);
	ctx->stacksize = 8;

	if (ctx_stack != NULL) {
		/* inherit old stacks */
		ctx->action_list_stack = ((config_parser_context_t*) ctx_stack->data)->action_list_stack;
		ctx->option_stack = ((config_parser_context_t*) ctx_stack->data)->option_stack;
		ctx->condition_stack = ((config_parser_context_t*) ctx_stack->data)->condition_stack;

		ctx->action_blocks = ((config_parser_context_t*) ctx_stack->data)->action_blocks;
		ctx->uservars = ((config_parser_context_t*) ctx_stack->data)->uservars;
	}
	else {
		ctx->action_blocks = g_hash_table_new_full((GHashFunc) g_string_hash, (GEqualFunc) g_string_equal, NULL, NULL);
		ctx->uservars = g_hash_table_new_full((GHashFunc) g_string_hash, (GEqualFunc) g_string_equal, NULL, NULL);

		ctx->action_list_stack = g_queue_new();
		ctx->option_stack = g_queue_new();
		ctx->condition_stack = g_queue_new();
	}

	return ctx;
}

void config_parser_context_free(server *srv, config_parser_context_t *ctx, gboolean free_queues)
{
	g_free(ctx->stack);

	if (free_queues) {
		if (g_queue_get_length(ctx->action_list_stack) > 0) {
			action *a;
			while ((a = g_queue_pop_head(ctx->action_list_stack)))
				action_release(srv, a);
		}

		if (g_queue_get_length(ctx->option_stack) > 0) {
			option *o;
			while ((o = g_queue_pop_head(ctx->option_stack)))
				option_free(o);
		}

		g_queue_free(ctx->action_list_stack);
		g_queue_free(ctx->option_stack);
	}

	g_slice_free(config_parser_context_t, ctx);
}

gboolean config_parser_file(server *srv, GList *ctx_stack, const gchar *path) {
	config_parser_context_t *ctx;
	gboolean res;
	GError *err = NULL;

	ctx = config_parser_context_new(srv, ctx_stack);
	ctx->filename = (gchar*) path;

	if (!g_file_get_contents(path, &ctx->ptr, &ctx->len, &err))
	{
		/* could not read file */
		log_warning(srv, NULL, "could not read config file \"%s\". reason: \"%s\" (%d)", path, err->message, err->code);
		config_parser_context_free(srv, ctx, FALSE);
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
	config_parser_context_free(srv, ctx, FALSE);

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

	ctx = config_parser_context_new(srv, ctx_stack);
	ctx->filename = (gchar*) command;

	if (!g_spawn_command_line_sync(command, &_stdout, &_stderr, &status, &err))
	{
		log_warning(srv, NULL, "error launching shell command \"%s\": %s (%d)", command, err->message, err->code);
		config_parser_context_free(srv, ctx, FALSE);
		g_error_free(err);
		return FALSE;
	}

	if (status != 0)
	{
		log_warning(srv, NULL, "shell command \"%s\" exited with status %d", command, status);
		log_debug(srv, NULL, "stdout:\n-----\n%s\n-----\nstderr:\n-----\n%s\n-----", _stdout, _stderr);
		g_free(_stdout);
		g_free(_stderr);
		config_parser_context_free(srv, ctx, FALSE);
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
	config_parser_context_free(srv, ctx, FALSE);

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

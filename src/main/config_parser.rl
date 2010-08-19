#include <lighttpd/base.h>
#include <lighttpd/config_parser.h>

#ifdef HAVE_LUA_H
# include <lighttpd/config_lua.h>
#endif

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
		/* _printf("current stacksize: %d, top: %d\n", ctx->stacksize, ctx->top); */
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
		liValue *o;

		o = li_value_new_bool(*ctx->mark == 't' ? TRUE : FALSE);
		g_queue_push_head(ctx->option_stack, o);

		_printf("got boolean %s in line %zd\n", *ctx->mark == 't' ? "true" : "false", ctx->line);
	}

	action integer {
		liValue *o;
		gint64 i = 0;

		for (gchar *c = ctx->mark; c < fpc; c++)
			i = i * 10 + *c - 48;

		o = li_value_new_number(i);
		/* push value onto stack */
		g_queue_push_head(ctx->option_stack, o);

		_printf("got integer %" G_GINT64_FORMAT " in line %zd\n", i, ctx->line);
	}

	action integer_suffix {
		liValue *o;
		GString *str;

		o = g_queue_peek_head(ctx->option_stack);

		str = g_string_new_len(ctx->mark, fpc - ctx->mark);

		     if (g_str_equal(str->str, "kbyte")) o->data.number *= 1024;
		else if (g_str_equal(str->str, "mbyte")) o->data.number *= 1024 * 1024;
		else if (g_str_equal(str->str, "gbyte")) o->data.number *= 1024 * 1024 * 1024;
		else if (g_str_equal(str->str, "tbyte")) o->data.number *= 1024 * 1024 * 1024 * G_GINT64_CONSTANT(1024);

		else if (g_str_equal(str->str, "kbit")) o->data.number *= 1024 / 8;
		else if (g_str_equal(str->str, "mbit")) o->data.number *= 1024 * 1024 / 8;
		else if (g_str_equal(str->str, "gbit")) o->data.number *= 1024 * 1024 * 1024 / 8;
		else if (g_str_equal(str->str, "tbit")) o->data.number *= 1024 * 1024 * 1024 * G_GINT64_CONSTANT(1024) / 8;

		else if (g_str_equal(str->str, "min")) o->data.number *= 60;
		else if (g_str_equal(str->str, "hours")) o->data.number *= 60 * 60;
		else if (g_str_equal(str->str, "days")) o->data.number *= 60 * 60 * 24;

		g_string_free(str, TRUE);

		_printf("got int with suffix: %" G_GINT64_FORMAT "\n", o->data.number);
	}

	action string {
		liValue *o;
		GString *str;
		gchar ch;

		str = g_string_sized_new(fpc - ctx->mark - 2);
		for (gchar *c = (ctx->mark+1); c != (fpc-1); c++) {
			if (*c != '\\')
				g_string_append_c(str, *c);
			else {
				guint avail = fpc - 1 - c;
				if (avail == 0) {
					ERROR(srv, "%s", "invalid \\ at end of string");
					g_string_free(str, TRUE);
					return FALSE;
				}

				switch (*(c+1)) {
				case '\\': g_string_append_c(str, '\\'); c++; break;
				case '"': g_string_append_c(str, '"'); c++; break;
				case 'n': g_string_append_c(str, '\n'); c++; break;
				case 'r': g_string_append_c(str, '\r'); c++; break;
				case 't': g_string_append_c(str, '\t'); c++; break;
				case 'x':
					if (avail < 3 || !(
						((*(c+2) >= '0' && *(c+2) <= '9') || (*(c+2) >= 'A' && *(c+2) <= 'F') || (*(c+2) >= 'a' && *(c+2) <= 'f')) &&
						((*(c+3) >= '0' && *(c+3) <= '9') || (*(c+3) >= 'A' && *(c+3) <= 'F') || (*(c+3) >= 'a' && *(c+3) <= 'f')))) {
						ERROR(srv, "%s", "invalid \\xHH in string");
						g_string_free(str, TRUE);
						return FALSE;
					}
					/* append char from hex */
					/* first char */
					if (*(c+2) <= '9')
						ch = 16 * (*(c+2) - '0');
					else if (*(c+2) <= 'F')
						ch = 16 * (10 + *(c+2) - 'A');
					else
						ch = 16 * (10 + *(c+2) - 'a');
					/* second char */
					if (*(c+3) <= '9')
						ch += *(c+3) - '0';
					else if (*(c+3) <= 'F')
						ch += 10 + *(c+3) - 'A';
					else
						ch += 10 + *(c+3) - 'a';
					c += 3;
					g_string_append_c(str, ch);
					break;
				default:
					g_string_append_c(str, '\\');
				}
			}
		}

		o = li_value_new_string(str);
		g_queue_push_head(ctx->option_stack, o);

		_printf("got string %s", "");
		for (gchar *c = ctx->mark + 1; c < fpc - 1; c++) _printf("%c", *c);
		_printf(" in line %zd\n", ctx->line);
	}

	# advanced types
	action list_start {
		liValue *o;

		/* create new list value and put it on stack, list entries are put in it by getting the previous value from the stack */
		o = li_value_new_list();
		g_queue_push_head(ctx->option_stack, o);

		fcall list_scanner;
	}

	action list_push {
		liValue *o, *l;

		/* pop current value from stack and append it to the new top of the stack value (the list) */
		o = g_queue_pop_head(ctx->option_stack);

		l = g_queue_peek_head(ctx->option_stack);
		assert(l->type == LI_VALUE_LIST);

		g_array_append_val(l->data.list, o);

		_printf("list_push %s\n", li_value_type_string(o->type));
	}

	action list_end {
		fret;
	}

	action hash_start {
		liValue *o;

		/* create new hash value and put it on stack, if a key-value pair is encountered, get it by walking 2 steps back the stack */
		o = li_value_new_hash();
		g_queue_push_head(ctx->option_stack, o);

		fcall hash_scanner;
	}

	action hash_push {
		liValue *k, *v, *h; /* key value hashtable */
		GString *str;

		v = g_queue_pop_head(ctx->option_stack);
		k = g_queue_pop_head(ctx->option_stack);
		h = g_queue_peek_head(ctx->option_stack);

		/* duplicate key so value can be free'd */
		str = g_string_new_len(k->data.string->str, k->data.string->len);

		g_hash_table_insert(h->data.hash, str, v);

		_printf("hash_push: %s: %s => %s\n", li_value_type_string(k->type), li_value_type_string(v->type), li_value_type_string(h->type));

		li_value_free(k);
	}

	action hash_end {
		fret;
	}

	action block_start {
		_printf("%s", "block_start\n");
		fcall block_scanner;
	}

	action block_end {
		_printf("%s", "block_end\n");
		fret;
	}

	action keyvalue_start {
		/* fpc--; */
		_printf("keyvalue start in line %zd\n", ctx->line);
		fcall key_value_scanner;
	}

	action keyvalue_end {
		liValue *k, *v, *l;
		/* we have a key and a value on the stack; convert them to a list with 2 elements */

		v = g_queue_pop_head(ctx->option_stack);
		k = g_queue_pop_head(ctx->option_stack);

		l = li_value_new_list();

		g_array_append_val(l->list, k);
		g_array_append_val(l->list, v);

		_printf("key-value pair: %s => %s in line %zd\n", li_value_type_string(k->type), li_value_type_string(v->type), ctx->line);

		/* push list on the stack */
		g_queue_push_head(ctx->option_stack, l);

		/* fpc--; */

		fret;
	}

	action liValue {
		liValue *o;

		o = g_queue_peek_head(ctx->option_stack);

		/* check if we need to cast the value */
		if (ctx->cast != LI_CFG_PARSER_CAST_NONE) {
			if (ctx->cast == LI_CFG_PARSER_CAST_INT) {
				/* cast string to integer */
				gint x = 0;
				guint i = 0;
				gboolean negative = FALSE;

				if (o->type != LI_VALUE_STRING) {
					ERROR(srv, "can only cast strings to integers, %s given", li_value_type_string(o->type));
					return FALSE;
				}

				if (o->data.string->str[0] == '-') {
					negative = TRUE;
					i++;
				}

				for (; i < o->data.string->len; i++) {
					gchar c = o->data.string->str[i];
					if (c < '0' || c > '9') {
						ERROR(srv, "%s", "cast(int) parameter doesn't look like a numerical string");
						return FALSE;
					}
					x = x * 10 + c - '0';
				}

				if (negative)
					x *= -1;

				g_string_free(o->data.string, TRUE);
				o->data.number = x;
				o->type = LI_VALUE_NUMBER;
			}
			else if (ctx->cast == LI_CFG_PARSER_CAST_STR) {
				/* cast integer to string */
				GString *str;

				if (o->type != LI_VALUE_NUMBER) {
					ERROR(srv, "can only cast integers to strings, %s given", li_value_type_string(o->type));
					return FALSE;
				}

				str = g_string_sized_new(0);
				g_string_printf(str, "%" G_GINT64_FORMAT, o->data.number);
				o->data.string = str;
				o->type = LI_VALUE_STRING;
			}

			ctx->cast = LI_CFG_PARSER_CAST_NONE;
		}

		_printf("value (%s) in line %zd\n", li_value_type_string(o->type), ctx->line);
	}

	action value_statement_start {
		fcall value_statement_scanner;
	}

	action value_statement_end {
		fret;
	}

	action value_statement_op {
		g_queue_push_head(ctx->value_op_stack, ctx->mark);
	}

	action value_statement {
		/* value (+|-|*|/) value */
		/* compute new value out of the two */
		liValue *l, *r, *o;
		gboolean free_l, free_r;
		gchar op;

		free_l = free_r = TRUE;

		r = g_queue_pop_head(ctx->option_stack);
		l = g_queue_pop_head(ctx->option_stack);
		o = NULL;

		op = *(gchar*)g_queue_pop_head(ctx->value_op_stack);

		if (op == '=') {
			/* value => value */
			free_l = FALSE;
			free_r = FALSE;
			o = li_value_new_list();
			g_array_append_val(o->data.list, l);
			g_array_append_val(o->data.list, r);
		}
		else if (l->type == LI_VALUE_NUMBER && r->type == LI_VALUE_NUMBER) {
			switch (op) {
				case '+': o = li_value_new_number(l->data.number + r->data.number); break;
				case '-': o = li_value_new_number(l->data.number - r->data.number); break;
				case '*': o = li_value_new_number(l->data.number * r->data.number); break;
				case '/': o = li_value_new_number(l->data.number / r->data.number); break;
			}
		}
		else if (l->type == LI_VALUE_STRING) {
			o = l;
			free_l = FALSE;

			if (r->type == LI_VALUE_STRING && op == '+') {
				/* str + str */
				o->data.string = g_string_append_len(o->data.string, GSTR_LEN(r->data.string));
			}
			else if (r->type == LI_VALUE_NUMBER && op == '+') {
				/* str + int */
				g_string_append_printf(o->data.string, "%" G_GINT64_FORMAT, r->data.number);
			}
			else if (r->type == LI_VALUE_NUMBER && op == '*') {
				/* str * int */
				if (r->data.number < 0) {
					ERROR(srv, "string multiplication with negative number (%" G_GINT64_FORMAT ")?", r->data.number);
					return FALSE;
				}
				else if (r->data.number == 0) {
					o->data.string = g_string_truncate(o->data.string, 0);
				}
				else {
					GString *str;
					str = g_string_new_len(l->data.string->str, l->data.string->len);
					for (gint i = 1; i < r->data.number; i++)
						o->data.string = g_string_append_len(o->data.string, str->str, str->len);
					g_string_free(str, TRUE);
				}
			}
			else
				o = NULL;
		}
		else if (l->type == LI_VALUE_LIST) {
			if (op == '+') {
				/* append r to the end of l */
				free_l = FALSE; /* use l as the new o */
				free_r = FALSE; /* r gets appended to o */
				o = l;

				g_array_append_val(l->data.list, r);
			}
			else if (op == '*') {
				/* merge l and r */
				if (r->type == LI_VALUE_LIST) {
					/* merge lists */
					free_l = FALSE;
					g_array_append_vals(l->data.list, r->data.list->data, r->data.list->len);
					g_array_set_size(r->data.list, 0);
					o = l;
				}
			}
		}
		else if (l->type == LI_VALUE_HASH && r->type == LI_VALUE_HASH && op == '+') {
			/* merge hashtables */
			GHashTableIter iter;
			gpointer key, val;
			free_l = FALSE; /* keep l, it's the new o */
			o = l;

			g_hash_table_iter_init(&iter, r->data.hash);
			while (g_hash_table_iter_next(&iter, &key, &val)) {
				g_hash_table_insert(o->data.hash, key, val);
				g_hash_table_iter_steal(&iter); /* steal key->value so it doesn't get deleted when destroying r */
			}
		}

		if (o == NULL) {
			WARNING(srv, "erronous value statement: %s %c %s in line %zd\n",
				li_value_type_string(l->type), op,
				li_value_type_string(r->type), ctx->line);
			return FALSE;
		}

		_printf("value statement: %s %c%s %s => %s in line %zd\n",
			li_value_type_string(l->type),
			op,
			op == '=' ?  ">" : "",
			li_value_type_string(r->type),
			li_value_type_string(o->type),
			ctx->line);

		if (free_l)
			li_value_free(l);
		if (free_r)
			li_value_free(r);

		g_queue_push_head(ctx->option_stack, o);
	}

	action varname {
		/* varname, push it as string value onto the stack */
		liValue *o;
		GString *str;

		str = g_string_new_len(ctx->mark, fpc - ctx->mark);
		o = li_value_new_string(str);
		g_queue_push_head(ctx->option_stack, o);
		_printf("got varname %s\n", str->str);
	}

	action actionref {
		/* varname is on the stack */
		liValue *o, *r, *t;

		o = g_queue_pop_head(ctx->option_stack);

		_printf("got actionref: %s in line %zd\n", o->data.string->str, ctx->line);

		/* action refs starting with "var." are user defined variables */
		if (g_str_has_prefix(o->data.string->str, "var.")) {
			/* look up var in hashtable, copy and push value onto stack */
			t = g_hash_table_lookup(ctx->uservars, o->data.string);

			if (t == NULL) {
				WARNING(srv, "unknown variable '%s'", o->data.string->str);
				li_value_free(o);
				return FALSE;
			}

			r = li_value_copy(t);
		}
		else if (g_str_has_prefix(o->data.string->str, "env.")) {
			/* look up string in environment, push value onto stack */
			gchar *env = getenv(o->data.string->str + 4);
			if (env == NULL) {
				ERROR(srv, "unknown environment variable: %s", o->data.string->str + 4);
				li_value_free(o);
				return FALSE;
			}

			r = li_value_new_string(g_string_new(env));
		}
		else {
			/* real action, lookup hashtable and create new action value */
			liAction *a;
			a = g_hash_table_lookup(ctx->action_blocks, o->data.string);

			if (a == NULL) {
				WARNING(srv,  "unknown action block referenced: %s", o->data.string->str);
				return FALSE;
			}

			li_action_acquire(a);
			r = li_value_new_action(srv, a);
		}

		g_queue_push_head(ctx->option_stack, r);
		li_value_free(o);
	}

	action operator {
		if ((fpc - ctx->mark) == 1) {
			switch (*ctx->mark) {
				case '<': ctx->op = LI_CONFIG_COND_LT; break;
				case '>': ctx->op = LI_CONFIG_COND_GT; break;
			}
		}
		else {
			     if (*ctx->mark == '>' && *(ctx->mark+1) == '=') ctx->op = LI_CONFIG_COND_GE;
			else if (*ctx->mark == '<' && *(ctx->mark+1) == '=') ctx->op = LI_CONFIG_COND_LE;
			else if (*ctx->mark == '=' && *(ctx->mark+1) == '=') ctx->op = LI_CONFIG_COND_EQ;
			else if (*ctx->mark == '!' && *(ctx->mark+1) == '=') ctx->op = LI_CONFIG_COND_NE;
			else if (*ctx->mark == '=' && *(ctx->mark+1) == '^') ctx->op = LI_CONFIG_COND_PREFIX;
			else if (*ctx->mark == '!' && *(ctx->mark+1) == '^') ctx->op = LI_CONFIG_COND_NOPREFIX;
			else if (*ctx->mark == '=' && *(ctx->mark+1) == '$') ctx->op = LI_CONFIG_COND_SUFFIX;
			else if (*ctx->mark == '!' && *(ctx->mark+1) == '$') ctx->op = LI_CONFIG_COND_NOSUFFIX;
			else if (*ctx->mark == '=' && *(ctx->mark+1) == '~') ctx->op = LI_CONFIG_COND_MATCH;
			else if (*ctx->mark == '!' && *(ctx->mark+1) == '~') ctx->op = LI_CONFIG_COND_NOMATCH;
			else if (*ctx->mark == '=' && *(ctx->mark+1) == '/') ctx->op = LI_CONFIG_COND_IP;
			else if (*ctx->mark == '!' && *(ctx->mark+1) == '/') ctx->op = LI_CONFIG_COND_NOTIP;
		}
	}

	# statements
	action assignment {
		liValue *val, *name;
		liAction *a, *al;

		/* top of the stack is the value, then the varname as string value */
		val = g_queue_pop_head(ctx->option_stack);
		name = g_queue_pop_head(ctx->option_stack);

		assert(name->type == LI_VALUE_STRING);

		_printf("got assignment: %s = %s; in line %zd\n", name->data.string->str, li_value_type_string(val->type), ctx->line);

		if (g_str_has_prefix(name->data.string->str, "var.")) {
			/* assignment vor user defined variable, insert into hashtable */
			gpointer old_key;
			gpointer old_val;
			GString *str = li_value_extract_string(name);

			/* free old key and value if we are overwriting it */
			if (g_hash_table_lookup_extended(ctx->uservars, str, &old_key, &old_val)) {
				g_hash_table_remove(ctx->uservars, str);
				g_string_free(old_key, TRUE);
				li_value_free(old_val);
			}

			g_hash_table_insert(ctx->uservars, str, val);
		}
		else if (ctx->in_setup_block) {
			/* in setup { } block, override default values for options */

			if (!li_plugin_set_default_option(srv, name->data.string->str, val)) {
				ERROR(srv, "failed overriding default value for option \"%s\"", name->data.string->str);
				li_value_free(name);
				li_value_free(val);
				return FALSE;
			}

			li_value_free(val);
		}
		else {
			/* normal assignment */
			a = li_option_action(srv, srv->main_worker, name->data.string->str, val);
			li_value_free(val);

			if (a == NULL) {
				li_value_free(name);
				return FALSE;
			}

			al = g_queue_peek_head(ctx->action_list_stack);
			g_array_append_val(al->data.list, a);
		}

		li_value_free(name);
	}

	action function_noparam {
		liValue *name;
		liAction *a, *al;

		name = g_queue_pop_head(ctx->option_stack);

		assert(name->type == LI_VALUE_STRING);

		_printf("got function: %s; in line %zd\n", name->data.string->str, ctx->line);

		if (g_str_equal(name->data.string->str, "break")) {
		}
		else if (g_str_equal(name->data.string->str, "__halt")) {
		}
		else {
			if (ctx->in_setup_block) {
				/* we are in the setup { } block, call setups and don't append to action list */
				if (!li_call_setup(srv, name->data.string->str, NULL)) {
					li_value_free(name);
					return FALSE;
				}
			}
			else {
				/* lookup hashtable of defined actions */
				a = g_hash_table_lookup(ctx->action_blocks, name->data.string);

				if (a == NULL) {
					a = li_create_action(srv, srv->main_worker, name->data.string->str, NULL);
				} else {
					li_action_acquire(a);
				}

				if (a == NULL) {
					li_value_free(name);
					return FALSE;
				}

				if (a == NULL) {
					li_value_free(name);
					return FALSE;
				}

				al = g_queue_peek_head(ctx->action_list_stack);
				g_array_append_val(al->data.list, a);
			}
		}

		li_value_free(name);
	}

	action function_param {
		/* similar to assignment */
		liValue *val, *name;
		liAction *a, *al;

		/* top of the stack is the value, then the varname as string value */
		val = g_queue_pop_head(ctx->option_stack);
		name = g_queue_pop_head(ctx->option_stack);

		assert(name->type == LI_VALUE_STRING);

		_printf("got function: %s %s; in line %zd\n", name->data.string->str, li_value_type_string(val->type), ctx->line);

		if (g_str_equal(name->data.string->str, "include")) {
			GPatternSpec *pattern;
			GDir *dir;
			gchar *pos;
			const gchar *filename;
			GString *path;
			guint len;
			GError *err = NULL;

			li_value_free(name);

			if (val->type != LI_VALUE_STRING || val->data.string->len == 0) {
				WARNING(srv,  "include directive takes a non-empty string as parameter, %s given", li_value_type_string(val->type));
				li_value_free(val);
				return FALSE;
			}

			/* split path into dirname and filename/pattern. e.g. /etc/lighttpd/vhost_*.conf => /etc/lighttpd/ and vhost_*.conf */

			if (val->data.string->str[0] == G_DIR_SEPARATOR) {
				/* absolute path */
				pos = strrchr(val->data.string->str, G_DIR_SEPARATOR);
				path = g_string_new_len(val->data.string->str, pos - val->data.string->str + 1);
				pattern = g_pattern_spec_new(pos+1);
			} else {
				/* relative path */
				pos = strrchr(ctx->filename, G_DIR_SEPARATOR);

				if (pos) {
					path = g_string_new_len(ctx->filename, pos - ctx->filename + 1);
				} else {
					/* current working directory */
					path = g_string_new_len(CONST_STR_LEN("." G_DIR_SEPARATOR_S));
				}

				pos = strrchr(val->data.string->str, G_DIR_SEPARATOR);

				if (pos) {
					g_string_append_len(path, val->data.string->str, pos - val->data.string->str + 1);
					pattern = g_pattern_spec_new(pos+1);
				} else {
					pattern = g_pattern_spec_new(val->data.string->str);
				}
			}

			/* we got a path, check for matching names */
			dir = g_dir_open(path->str, 0, &err);

			if (!dir) {
				ERROR(srv, "include: could not open directory \"%s\": %s", path->str, err->message);
				g_string_free(path, TRUE);
				li_value_free(val);
				g_error_free(err);
				g_pattern_spec_free(pattern);
				return FALSE;
			}

			len = path->len;

			/* loop through all filenames in the directory and include matching ones */
			while (NULL != (filename = g_dir_read_name(dir))) {
				if (!g_pattern_match_string(pattern, filename))
					continue;

				g_string_append(path, filename);

				if (!li_config_parser_file(srv, ctx_stack, path->str)) {
					g_string_free(path, TRUE);
					g_pattern_spec_free(pattern);
					g_dir_close(dir);
					li_value_free(val);
					return FALSE;
				}

				g_string_truncate(path, len);
			}

			g_string_free(path, TRUE);
			g_pattern_spec_free(pattern);
			g_dir_close(dir);
			li_value_free(val);
		}
		else if (g_str_equal(name->data.string->str, "include_shell")) {
			if (val->type != LI_VALUE_STRING) {
				WARNING(srv, "include_shell directive takes a string as parameter, %s given", li_value_type_string(val->type));
				li_value_free(name);
				li_value_free(val);
				return FALSE;
			}

			if (!config_parser_shell(srv, ctx_stack, val->data.string->str)) {
				li_value_free(name);
				li_value_free(val);
				return FALSE;
			}

			li_value_free(val);
		}
#ifdef HAVE_LUA_H
		else if (g_str_equal(name->data.string->str, "include_lua")) {
			if (val->type != LI_VALUE_STRING) {
				WARNING(srv, "include_lua directive takes a string as parameter, %s given", li_value_type_string(val->type));
				li_value_free(name);
				li_value_free(val);
				return FALSE;
			}

			if (!li_config_lua_load(srv->L, srv, srv->main_worker, val->data.string->str, &a, TRUE, NULL)) {
				ERROR(srv, "include_lua '%s' failed", val->data.string->str);
				li_value_free(name);
				li_value_free(val);
				return FALSE;
			}
			li_value_free(name);
			li_value_free(val);

			/* include lua doesn't need to produce an action */
			if (a != NULL) {
				al = g_queue_peek_head(ctx->action_list_stack);
				g_array_append_val(al->data.list, a);
			}
		}
#endif
		/* internal functions */
		else if (g_str_has_prefix(name->data.string->str, "__")) {
			if (g_str_equal(name->data.string->str + 2, "print")) {
				GString *tmpstr = li_value_to_string(val);
				DEBUG(srv, "%s:%zd type: %s, value: %s", ctx->filename, ctx->line, li_value_type_string(val->type), tmpstr->str);
				g_string_free(tmpstr, TRUE);
			}

			li_value_free(val);
			li_value_free(name);
		}
		/* normal function action */
		else {
			if (ctx->in_setup_block) {
				/* we are in the setup { } block, call setups and don't append to action list */
				if (!li_call_setup(srv, name->data.string->str, val)) {
					li_value_free(name);
					li_value_free(val);
					return FALSE;
				}

				li_value_free(val);
			}
			else {
				al = g_queue_peek_head(ctx->action_list_stack);
				a = li_create_action(srv, srv->main_worker, name->data.string->str, val);
				li_value_free(val);

				if (a == NULL) {
					li_value_free(name);
					return FALSE;
				}

				g_array_append_val(al->data.list, a);
			}

			li_value_free(name);
		}
	}

	action action_block_start {
		liValue *o;
		liAction *al;

		o = g_queue_pop_head(ctx->option_stack);
		assert(o->type == LI_VALUE_STRING);

		if (ctx->in_setup_block) {
			ERROR(srv, "%s", "no block inside the setup block allowed");
			return FALSE;
		}

		if (g_str_equal(o->data.string->str, "setup")) {
			_printf("entered setup block in line %zd\n", ctx->line);
			ctx->in_setup_block = TRUE;
		}
		else {
			GString *str;

			_printf("action block %s in line %zd\n", o->data.string->str, ctx->line);

			/* create new action list and put it on the stack */
			al = li_action_new_list();
			g_queue_push_head(ctx->action_list_stack, al);
			/* insert into hashtable for later lookups */
			str = g_string_new_len(o->data.string->str, o->data.string->len);
			g_hash_table_insert(ctx->action_blocks, str, al);
		}

		li_value_free(o);
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

	action action_block_noname_start {
		liAction *al;

		if (ctx->in_setup_block) {
			ERROR(srv, "%s", "no block inside the setup block allowed");
			return FALSE;
		}

		_printf("action block in line %zd\n", ctx->line);

		/* create new action list and put it on the stack */
		al = li_action_new_list();
		g_queue_push_head(ctx->action_list_stack, al);
	}

	action action_block_noname_end {
		liValue *v;
		liAction *a;

		/* pop action list stack */
		a = g_queue_pop_head(ctx->action_list_stack);
		v = li_value_new_action(srv, a);
		g_queue_push_head(ctx->option_stack, v);
	}

	action cond_key { ctx->condition_with_key = TRUE; _printf("%s", "cond_key\n"); }
	action cond_and_or {
		_printf("%s", "and_or: ");
		for (gchar *c = ctx->mark; c < fpc; c++) _printf("%c", *c);
		_printf("%s", "\n");
		/* we use spacial values 0x1 and 0x2 to indicate "and" or "or" on the condition stack */
		if (strncmp(ctx->mark, "and", fpc - ctx->mark) == 0) {
			/* we got an 'or', push special value 0x1 on condition stack */
			g_queue_push_head(ctx->condition_stack, GINT_TO_POINTER(0x1));
		} else {

			/* we got an 'or', push special value 0x2 on condition stack */
			g_queue_push_head(ctx->condition_stack, GINT_TO_POINTER(0x2));
		}
	}
	action condition {
		/* stack: value, varname OR value, key, varname */
		liValue *val, *varname, *key;
		liCondition *cond;
		liConditionLValue *lvalue;
		liCondLValue lvalue_type;

		_printf("%s", "condition\n");

		/* if condition is nonbool, then it has a value and maybe a key too on the stack */
		if (ctx->condition_nonbool) {
			val = g_queue_pop_head(ctx->option_stack);
			if (ctx->condition_with_key)
				key = g_queue_pop_head(ctx->option_stack);
			else
				key = NULL;
		} else {
			val = NULL;
			key = NULL;
		}

		/* pop varname from stack, this will be the lval */
		varname = g_queue_pop_head(ctx->option_stack);
		assert(varname->type == LI_VALUE_STRING);

		if (ctx->condition_nonbool) {
			if (ctx->condition_with_key) {
				_printf("got condition: %s[%s] %s %s in line %zd\n", varname->data.string->str, key->data.string->str, li_comp_op_to_string(ctx->op), li_value_type_string(val->type), ctx->line);
			} else {
				_printf("got condition: %s %s %s in line %zd\n", varname->data.string->str, li_comp_op_to_string(ctx->op), li_value_type_string(val->type), ctx->line);
			}
		} else {
			if (ctx->condition_with_key) {
				_printf("got condition: %s[%s] in line %zd\n", varname->data.string->str, key->data.string->str, ctx->line);
			} else {
				_printf("got condition: %s in line %zd\n", varname->data.string->str, ctx->line);
			}
		}

		/* create condition lvalue */
		lvalue_type = li_cond_lvalue_from_string(GSTR_LEN(varname->data.string));

		if (lvalue_type == LI_COMP_UNKNOWN) {
			ERROR(srv, "unknown lvalue for condition: %s", varname->data.string->str);
			return FALSE;
		}

		if ((lvalue_type == LI_COMP_REQUEST_HEADER || lvalue_type == LI_COMP_ENVIRONMENT || lvalue_type == LI_COMP_RESPONSE_HEADER) && key == NULL) {
			ERROR(srv, "%s conditional needs a key", varname->data.string->str);
			return FALSE;
		}

		lvalue = li_condition_lvalue_new(lvalue_type, key ? li_value_extract_string(key) : NULL);

		/* if condition is non-boolean, then we have a rval */
		if (ctx->condition_nonbool) {
			if (val->type == LI_VALUE_STRING) {
				cond = li_condition_new_string(srv, ctx->op, lvalue, li_value_extract_string(val));
			} else if (val->type == LI_VALUE_NUMBER) {
				cond = li_condition_new_int(srv, ctx->op, lvalue, val->data.number);
			} else {
				cond = NULL;
			}
		} else {
			/* boolean condition */
			cond = li_condition_new_bool(srv, lvalue, !ctx->condition_negated);
		}

		if (cond == NULL) {
			ERROR(srv, "%s", "could not create condition");
			return FALSE;
		}

		g_queue_push_head(ctx->condition_stack, cond);

		li_value_free(varname);
		if (ctx->condition_nonbool) {
			li_value_free(key);
			li_value_free(val);
		}

		ctx->condition_with_key = FALSE;
		ctx->condition_nonbool = FALSE;
		ctx->condition_negated = FALSE;
	}
	action conditions {
		liAction *a, *target_action, *action_list, *action_last_and;
		liCondition *cond, *cond_next;

		cond = g_queue_pop_head(ctx->condition_stack);
		target_action = g_queue_pop_head(ctx->action_list_stack);
		action_list = g_queue_peek_head(ctx->action_list_stack);

		a = li_action_new_condition(cond, target_action, NULL);
		action_last_and = a;
		_printf("new condition action: %p, target: %p\n", (void*)a, (void*)target_action);

		while (NULL != (cond_next = g_queue_peek_head(ctx->condition_stack))) {
			if (cond_next == GINT_TO_POINTER(0x1)) {
				/* 'and' */
				g_queue_pop_head(ctx->condition_stack);
				cond = g_queue_pop_head(ctx->condition_stack);
				action_last_and = a;
				/* mark target pointer as 'and' */
				a = li_action_new_condition(cond, (liAction*)((uintptr_t)a | 0x1), NULL);
				_printf("new AND condition action: %p, target: %p, else: %p\n", (void*)a, (void*)action_last_and, NULL);
				action_last_and = a;
			} else if (cond_next == GINT_TO_POINTER(0x2)) {
				/* 'or' */
				g_queue_pop_head(ctx->condition_stack);
				cond = g_queue_pop_head(ctx->condition_stack);
				a = li_action_new_condition(cond, target_action, action_last_and);
				_printf("new OR condition action: %p, target: %p, else: %p\n", (void*)a, (void*)target_action, (void*)action_last_and);
			} else {
				break;
			}
		}

		g_array_append_val(action_list->data.list, a);
	}

	action cond_else {
		/*
			else block WITHOUT condition
			- pop current action list from stack
			- peek previous action list from stack
			- get last action from action list
			- put current action list as target_else of the last action
		*/
		liAction *action_list, *target, *cond, *cond_and;

		target = g_queue_pop_head(ctx->action_list_stack);
		action_list = g_queue_peek_head(ctx->action_list_stack);
		/* last action in the list is our condition */
		cond = g_array_index(action_list->data.list, liAction*, action_list->data.list->len - 1);

		/* loop over all actions until we find the last without target_else */
		while (TRUE) {
			for (cond_and = cond; (uintptr_t)cond_and->data.condition.target & 0x1; cond_and = cond_and->data.condition.target) {
				cond_and->data.condition.target = (liAction*)((uintptr_t)cond_and->data.condition.target & (~0x1));
				cond_and->data.condition.target->data.condition.target_else = target;
			}

			if (cond->data.condition.target_else)
				cond = cond->data.condition.target_else;
			else
				break;
		}

		cond->data.condition.target_else = target;
		_printf("got cond_else in line %zd\n", ctx->line);
	}

	action cond_else_if {
		/*
			else block WITH condition
			- peek current action list from stack
			- get last action from action list, this is our target action
			- get second last action from action list, this is the condition to modify
			- put target action as target_else of the last condition
		*/
		liAction *action_list, *target, *cond, *cond_and;

		/* remove current condition action from list and put it as the else target of the previous condition action */
		action_list = g_queue_peek_head(ctx->action_list_stack);
		target = g_array_index(action_list->data.list, liAction*, action_list->data.list->len - 1);
		cond = g_array_index(action_list->data.list, liAction*, action_list->data.list->len - 2);
		g_array_remove_index(action_list->data.list, action_list->data.list->len - 1);

		/* loop over all actions until we find the last without target_else */
		while (TRUE) {
			for (cond_and = cond; (uintptr_t)cond_and->data.condition.target & 0x1; cond_and = cond_and->data.condition.target) {

				cond_and->data.condition.target = (liAction*)((uintptr_t)cond_and->data.condition.target & (~0x1));
				cond_and->data.condition.target->data.condition.target_else = target;
			}

			if (cond->data.condition.target_else)
				cond = cond->data.condition.target_else;
			else
				break;
		}

		cond->data.condition.target_else = target;
		_printf("got cond_else_if in line %zd\n", ctx->line);
	}

	action condition_chain {
		liAction *action_list, *cond, *cond_and;
		action_list = g_queue_peek_head(ctx->action_list_stack);
		/* last action in the list is our condition */
		cond = g_array_index(action_list->data.list, liAction*, action_list->data.list->len - 1);

		_printf("got condition_chain in line %zd\n", ctx->line);

		/* loop over all actions looking for 'and' markers and clear them */
		while (cond && cond->type == LI_ACTION_TCONDITION) {
			_printf("condition: %p if: %p else: %p\n", (void*)cond, (void*)cond->data.condition.target, (void*)cond->data.condition.target_else);
			for (cond_and = cond; cond_and && (cond_and->type == LI_ACTION_TCONDITION) &&  (uintptr_t)cond_and->data.condition.target & 0x1; cond_and = cond_and->data.condition.target) {
				cond_and->data.condition.target = (liAction*)((uintptr_t)cond_and->data.condition.target & (~0x1));
				_printf("condition_and: %p if: %p else: %p\n", (void*)cond_and, (void*)cond_and->data.condition.target, (void*)cond_and->data.condition.target_else);
			}

			if (cond->data.condition.target_else)
				cond = cond->data.condition.target_else;
			else
				break;
		}
	}


	## definitions

	# misc stuff
	line_sane = ( '\n' ) >{ ctx->line++; };
	line_weird = ( '\r' ) >{ ctx->line++; };
	line_insane = ( '\r' ( '\n' >{ ctx->line--; } ) );
	line = ( line_sane | line_weird | line_insane );

	ws = ( '\t' | ' ' );
	comment = ( '#' (any - line)* line );
	noise = ( ws | line | comment );

	block = ( '{' >block_start );

	# basic types
	boolean = ( 'true' | 'false' ) %boolean;
	integer_suffix_bytes = ( 'byte' | 'kbyte' | 'mbyte' | 'gbyte' | 'tbyte' | 'pbyte' );
	integer_suffix_bits = ( 'bit' | 'kbit' | 'mbit' | 'gbit' | 'tbit' | 'pbit' );
	integer_suffix_seconds = ( 'sec' | 'min' | 'hours' | 'days' );
	integer_suffix = ( integer_suffix_bytes | integer_suffix_bits | integer_suffix_seconds ) >mark %integer_suffix;
	integer = ( ('0' | ( [1-9] [0-9]* )) %integer integer_suffix? );
	escaped_hex = ( '\\x' xdigit{2} );
	special_chars = ( '\\' [nrt\\] );
	#string = ( '"' ( ( any - ["\\] ) | special_chars | escaped_hex | '\\"' )* '"' ) %string;
	#string = ( '"' ( ( any - ["\\] ) | '\\"' )* '"' ) %string;
	string = ( '"' ( ( any - ["\\] ) | escaped_hex | ( '\\' ( any - [x]) ) )* '"' ) %string;

	# casts
	cast = ( 'cast(' ( 'int' %{ctx->cast = LI_CFG_PARSER_CAST_INT;} | 'str' %{ctx->cast = LI_CFG_PARSER_CAST_STR;} ) ')' ws* );

	keywords = ( 'true' | 'false' | 'if' | 'else' );

	# advanced types
	varname = ( '__' ? (alpha ( alnum | [._] )*) - keywords ) >mark %varname;
	actionref = ( varname ) %actionref;
	list = ( '(' >list_start );
	hash = ( '[' >hash_start );

	action_block = ( varname noise* block >action_block_start ) %action_block_end;
	action_block_noname = ( '$' block > action_block_noname_start ) %action_block_noname_end;

	value = ( ( boolean | integer | string | list | hash | actionref | action_block_noname ) >mark ) %liValue;
	value_statement_op = ( '+' | '-' | '*' | '/' | '=>' ) >mark >value_statement_op;
	value_statement = ( noise* cast? value (ws* value_statement_op ws* cast? value %value_statement)* noise* );
	hash_elem = ( noise* string >mark noise* ':' value_statement );

	# conditions
	cond_lval = ( varname );
	cond_rval = ( value_statement ) %{ ctx->condition_nonbool = TRUE; };
	cond_negated = ( '!' ) %{ ctx->condition_negated = TRUE; };
	cond_key = ( '[' string >mark ']' ) %cond_key;
	cond_operator = ( '==' | '!=' | '=^' | '!^' | '=$' | '!$' | '<' | '<=' | '>' | '>=' | '=~' | '!~' | '=/' | '!/' ) >mark %operator;
	cond_and_or = ( 'and' | 'or' ) >mark %cond_and_or;
	condition = ( cond_negated? cond_lval cond_key? ws+ ( cond_operator ws+ cond_rval  )? ) <: '' %condition;
	conditions = ( 'if' noise+ condition ( cond_and_or noise+ condition )* block >action_block_noname_start ) %conditions;
	cond_else_if = ( 'else' noise+ conditions ) %cond_else_if;
	cond_else = ( 'else' noise+ block >action_block_noname_start ) %cond_else;
	condition_chain = ( conditions (noise+ cond_else_if)* (noise+ cond_else)? ) %condition_chain;

	# statements
	assignment = ( varname ws* '=' ws* value_statement ';' ) %assignment;
	function_noparam = ( varname ws* ';' ) %function_noparam;
	function_param = ( varname ws+ value_statement ';') %function_param;
	function = ( function_noparam | function_param );
	statement = ( assignment | function | condition_chain | action_block );

	# scanner
	list_scanner := ( ((value_statement %list_push ( ',' value_statement %list_push )*)  | noise*) ')' >list_end );
	hash_scanner := ( ((hash_elem %hash_push ( ',' hash_elem %hash_push )*) | noise*) ']' >hash_end );
	block_scanner := ( (noise | statement)* '}' >block_end );

	main := (noise | statement)* '\00';
}%%

%% write data;

static liConfigParserContext *config_parser_context_new(liServer *srv, GList *ctx_stack) {
	liConfigParserContext *ctx;

	UNUSED(srv);

	ctx = g_slice_new0(liConfigParserContext);

	ctx->line = 1;

	/* allocate stack of 8 items. sufficient for most configs, will grow when needed */
	ctx->stack = (int*) g_malloc(sizeof(int) * 8);
	ctx->stacksize = 8;

	if (ctx_stack != NULL) {
		/* inherit old stacks */
		ctx->action_list_stack = ((liConfigParserContext*) ctx_stack->data)->action_list_stack;
		ctx->option_stack = ((liConfigParserContext*) ctx_stack->data)->option_stack;
		ctx->condition_stack = ((liConfigParserContext*) ctx_stack->data)->condition_stack;
		ctx->value_op_stack = ((liConfigParserContext*) ctx_stack->data)->value_op_stack;

		ctx->action_blocks = ((liConfigParserContext*) ctx_stack->data)->action_blocks;
		ctx->uservars = ((liConfigParserContext*) ctx_stack->data)->uservars;
	}
	else {
		GString *str;
		liValue *o;
		ctx->action_blocks = g_hash_table_new_full((GHashFunc) g_string_hash, (GEqualFunc) g_string_equal, NULL, NULL);
		ctx->uservars = g_hash_table_new_full((GHashFunc) g_string_hash, (GEqualFunc) g_string_equal, NULL, NULL);

		/* initialize var.PID */
		/* TODO: what if pid_t is not a 32bit integer? */
		o = li_value_new_number(getpid());
		str = g_string_new_len(CONST_STR_LEN("var.PID"));
			g_hash_table_insert(ctx->uservars, str, o);

		/* initialize var.CWD */
		str = g_string_sized_new(1023);
		if (NULL != getcwd(str->str, 1022)) {
			g_string_set_size(str, strlen(str->str));
			o = li_value_new_string(str);
			str = g_string_new_len(CONST_STR_LEN("var.CWD"));
			g_hash_table_insert(ctx->uservars, str, o);
		}
		else
			g_string_free(str, TRUE);

		ctx->action_list_stack = g_queue_new();
		ctx->option_stack = g_queue_new();
		ctx->condition_stack = g_queue_new();
		ctx->value_op_stack = g_queue_new();
	}

	return ctx;
}

static void config_parser_context_free(liServer *srv, liConfigParserContext *ctx, gboolean free_queues) {
	g_free(ctx->stack);

	if (free_queues) {
		if (g_queue_get_length(ctx->option_stack) > 0) {
			liValue *o;
			while ((o = g_queue_pop_head(ctx->option_stack)))
				li_value_free(o);
		}

		if (g_queue_get_length(ctx->condition_stack) > 0) {
			liCondition *c;
			while ((c = g_queue_pop_head(ctx->condition_stack)))
				li_condition_release(srv, c);
		}

		g_queue_free(ctx->action_list_stack);
		g_queue_free(ctx->option_stack);
		g_queue_free(ctx->condition_stack);
		g_queue_free(ctx->value_op_stack);
	}

	g_slice_free(liConfigParserContext, ctx);
}

GList* li_config_parser_init(liServer* srv) {
	liConfigParserContext *ctx = config_parser_context_new(srv, NULL);

	srv->mainaction = li_action_new_list();
	g_queue_push_head(ctx->action_list_stack, srv->mainaction);

	return g_list_append(NULL, ctx);
}

void li_config_parser_finish(liServer *srv, GList *ctx_stack, gboolean free_all) {
	liConfigParserContext *ctx;
	GHashTableIter iter;
	gpointer key, val;
	GList *l;

	_printf("ctx_stack size: %u\n", g_list_length(ctx_stack));

	/* clear all contexts from the stack */
	l = g_list_nth(ctx_stack, 1);
	while (l) {
		ctx = l->data;
		config_parser_context_free(srv, ctx, FALSE);
		l = l->next;
	}

	if (free_all) {
		ctx = (liConfigParserContext*) ctx_stack->data;

		g_hash_table_iter_init(&iter, ctx->action_blocks);

		while (g_hash_table_iter_next(&iter, &key, &val)) {
			li_action_release(srv, val);
			g_string_free(key, TRUE);
		}

		g_hash_table_destroy(ctx->action_blocks);

		g_hash_table_iter_init(&iter, ctx->uservars);

		while (g_hash_table_iter_next(&iter, &key, &val)) {
			li_value_free(val);
			g_string_free(key, TRUE);
		}

		g_hash_table_destroy(ctx->uservars);



		config_parser_context_free(srv, ctx, TRUE);

		g_list_free(ctx_stack);
	}
}

static gboolean config_parser_buffer(liServer *srv, GList *ctx_stack);

gboolean li_config_parser_file(liServer *srv, GList *ctx_stack, const gchar *path) {
	liConfigParserContext *ctx;
	gboolean res;
	GError *err = NULL;

	ctx = config_parser_context_new(srv, ctx_stack);
	ctx->filename = (gchar*) path;

	if (!g_file_get_contents(path, &ctx->ptr, &ctx->len, &err)) {
		/* could not read file */
		WARNING(srv, "could not read config file \"%s\". reason: \"%s\" (%d)", path, err->message, err->code);
		config_parser_context_free(srv, ctx, FALSE);
		g_error_free(err);
		return FALSE;
	}

	/* push on stack */
	ctx_stack = g_list_prepend(ctx_stack, ctx);

	res = config_parser_buffer(srv, ctx_stack);

	if (!res)
		WARNING(srv, "config parsing failed in line %zd of %s", ctx->line, ctx->filename);

	/* pop from stack */
	ctx_stack = g_list_delete_link(ctx_stack, ctx_stack);

	/* have to free the buffer on our own */
	g_free(ctx->ptr);
	config_parser_context_free(srv, ctx, FALSE);

	return res;
}

static gboolean config_parser_shell(liServer *srv, GList *ctx_stack, const gchar *command) {
	gboolean res;
	gchar* _stdout;
	gchar* _stderr;
	gint status;
	liConfigParserContext *ctx;
	GError *err = NULL;

	ctx = config_parser_context_new(srv, ctx_stack);
	ctx->filename = (gchar*) command;

	if (!g_spawn_command_line_sync(command, &_stdout, &_stderr, &status, &err)) {
		WARNING(srv, "error launching shell command \"%s\": %s (%d)", command, err->message, err->code);
		config_parser_context_free(srv, ctx, FALSE);
		g_error_free(err);
		return FALSE;
	}

	if (status != 0) {
		WARNING(srv, "shell command \"%s\" exited with status %d", command, status);
		DEBUG(srv, "stdout:\n-----\n%s\n-----\nstderr:\n-----\n%s\n-----", _stdout, _stderr);
		g_free(_stdout);
		g_free(_stderr);
		config_parser_context_free(srv, ctx, FALSE);
		return FALSE;
	}

	ctx->len = strlen(_stdout);
	ctx->ptr = _stdout;

	DEBUG(srv, "included shell output from \"%s\" (%zu bytes)", command, ctx->len);

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

static gboolean config_parser_buffer(liServer *srv, GList *ctx_stack) {
	liConfigParserContext *ctx;

	/* get top of stack */
	ctx = (liConfigParserContext*) ctx_stack->data;

	ctx->p = ctx->ptr;
	ctx->pe = ctx->ptr + ctx->len + 1; /* marks the end of the data to scan (+1 because of trailing \0 char) */

	%% write init;

	%% write exec;

	if (ctx->cs == config_parser_error || ctx->cs == config_parser_first_final) {
		/* parse error */
		WARNING(srv, "parse error in line %zd of \"%s\" at character '%c' (0x%.2x)", ctx->line, ctx->filename, *ctx->p, *ctx->p);
		return FALSE;
	}

	return TRUE;
}

gboolean li_config_parse(liServer *srv, const gchar *config_path) {
	GTimeVal start, end;
	gulong s, millis, micros;
	guint64 d;
	liAction *a;
	liConfigParserContext *ctx;
	GList *ctx_stack = NULL;

	g_get_current_time(&start);

	/* standard config frontend */
	ctx_stack = li_config_parser_init(srv);
	ctx = (liConfigParserContext*) ctx_stack->data;
	if (!li_config_parser_file(srv, ctx_stack, config_path)) {
		li_config_parser_finish(srv, ctx_stack, TRUE);
		return FALSE; /* no cleanup on config error, same as config test */
	}

	/* append fallback "static" action */
	a = li_create_action(srv, srv->main_worker, "static", NULL);
	g_array_append_val(srv->mainaction->data.list, a);

	g_get_current_time(&end);
	d = end.tv_sec - start.tv_sec;
	d *= 1000000;
	d += end.tv_usec - start.tv_usec;
	s = d / 1000000;
	millis = (d - s) / 1000;
	micros = (d - s - millis) %1000;
	DEBUG(srv, "parsed config file in %lus, %lums, %luus", s, millis, micros);

	if (g_queue_get_length(ctx->option_stack) != 0 || g_queue_get_length(ctx->action_list_stack) != 1)
		DEBUG(srv, "option_stack: %u action_list_stack: %u (should be 0:1)", g_queue_get_length(ctx->option_stack), g_queue_get_length(ctx->action_list_stack));

	li_config_parser_finish(srv, ctx_stack, FALSE);

	li_config_parser_finish(srv, ctx_stack, TRUE);

	return TRUE;
}

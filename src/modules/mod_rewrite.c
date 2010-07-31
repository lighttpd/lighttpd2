/*
 * mod_rewrite - modify request path and querystring with support for regular expressions
 *
 * Description:
 *     mod_rewrite lets you modify (rewrite) the path and querystring of a request.
 *     It supports matching regular expressions and substitution with captured substrings as well as other placeholders.
 *     A so called rewrite rule consist of a regular expression and a target string.
 *
 *     If your rewrite target does not contain any questionmark (?), then the querystring will not be altered.
 *     If it does, then it will be overwritten. To append the original querystring, use %{request.query}.
 *
 *     Placeholders:
 *         - $1..9 replaced by captured substring of current regex
 *         - $0 replaced by whole string that matched the regex
 *         - %0..9 same as $n but uses regex from previous conditional
 *         - %{var} with var being one of the req.* or phys.* e.g. %{request.host}
 *           supported vars: request.host, request.path, request.query, request.remoteip, request.localip, request.content_length
 *         - %{enc:var} same as %{var} but urlencoded e.g. %{enc:request.path}
 *
 *     ?, $ and % can be escaped using \?, \$ and \% respectively.
 *
 * Setups:
 *     none
 * Options:
 *     rewrite.debug = <true|false>;
 *         - if set, debug information is written to the log
 * Actions:
 *     rewrite "/new/path";
 *         - sets request.path to "/new/path", substituting all placeholders. $0..$9 get replaced by empty strings.
 *     rewrite "regex" => "/new/path";
 *         - sets request.path to "/new/path" if "regex" matched the original req.path.
 *         - $0..$9 get replaced by the captured substrings of the regular expression "regex".
 *     rewrite ("regex1" => "/new/path1", ..., "regexN" => "/new/pathN");
 *         - traverses the list of rewrite rules.
 *         - rewrites request.path to the corresponding "/new/path" if the regex matches and stops traversing the list.
 *
 * Example config:
 *     rewrite (
 *         "^/article/(\d+)/.*$" => "/article.php?id=$1",
 *         "^/download/(\d+)/(.*)$" => "/download.php?fileid=$1&filename=$2"
 *     );
 *     rewrite "^/user/(.+)$" => "/user.php?name=$1";
 *
 *
 * Tip:
 *     As both config parser and regex compiler use backslashes to escape special characters, you will have to escape them twice.
 *     For example "^foo\\dbar$" will end up as "^foo\dbar$" as regex input, which would match things like "foo3bar".
 *
 * Todo:
 *     - implement rewrite_optimized which reorders rules according to hitcount
 *     - implement rewrite_raw which uses the raw uri
 *
 * Author:
 *     Copyright (c) 2009 Thomas Porzelt
 * License:
 *     MIT, see COPYING file in the lighttpd 2 tree
 */

#include <lighttpd/base.h>
#include <lighttpd/encoding.h>

LI_API gboolean mod_rewrite_init(liModules *mods, liModule *mod);
LI_API gboolean mod_rewrite_free(liModules *mods, liModule *mod);

struct rewrite_plugin_data {
	GPtrArray *tmp_strings; /* array of (GString*) */
};
typedef struct rewrite_plugin_data rewrite_plugin_data;

struct rewrite_part {
	enum {
		REWRITE_PART_STRING,
		REWRITE_PART_CAPTURED,
		REWRITE_PART_CAPTURED_PREV,
		REWRITE_PART_VAR,
		REWRITE_PART_VAR_ENCODED,
		REWRITE_PART_QUERYSTRING
	} type;
	union {
		GString *str;
		guint8 ndx;
		liCondLValue cond_lval;
	} data;
};
typedef struct rewrite_part rewrite_part;

struct rewrite_rule {
	GArray *parts;
	GRegex *regex;
	gboolean has_querystring;
};
typedef struct rewrite_rule rewrite_rule;

struct rewrite_data {
	GArray *rules;
	liPlugin *p;
};
typedef struct rewrite_data rewrite_data;



static void rewrite_parts_free(GArray *parts) {
	guint i;

	for (i = 0; i < parts->len; i++) {
		rewrite_part *rp = &g_array_index(parts, rewrite_part, i);

		switch (rp->type) {
		case REWRITE_PART_STRING: g_string_free(rp->data.str, TRUE); break;
		default: break;
		}
	}

	g_array_free(parts, TRUE);
}

static GArray *rewrite_parts_parse(GString *str, gboolean *has_querystring) {
	rewrite_part rp;
	gboolean encoded;
	gchar *c = str->str;
	GArray *parts = g_array_new(FALSE, FALSE, sizeof(rewrite_part));

	for (;;) {
		if (*c == '\0') {
			break;
		} else if (*c == '?') {
			/* querystring */
			rp.type = REWRITE_PART_QUERYSTRING;
			g_array_append_val(parts, rp);
			c++;
			*has_querystring = TRUE;
		} else if (*c == '$') {
			c++;
			if (*c >= '0' && *c <= '9') {
				/* $n backreference */
				rp.type = REWRITE_PART_CAPTURED;
				rp.data.ndx = *c - '0';
				g_array_append_val(parts, rp);
				c++;
			} else {
				/* parse error */
				rewrite_parts_free(parts);
				return NULL;
			}
		} else if (*c == '%') {
			c++;
			if (*c >= '0' && *c <= '9') {
				/* %n backreference */
				rp.type = REWRITE_PART_CAPTURED_PREV;
				rp.data.ndx = *c - '0';
				g_array_append_val(parts, rp);
				c++;
			} else if (*c == '{') {
				/* %{var} */
				guint len;

				c++;
				encoded = FALSE;

				if (g_str_has_prefix(c, "enc:")) {
					c += sizeof("enc:")-1;
					encoded = TRUE;
				}

				for (len = 0; *c != '\0' && *c != '}'; c++)
					len++;

				if (*c == '\0') {
					/* parse error */
					rewrite_parts_free(parts);
					return NULL;
				}

				rp.data.cond_lval = li_cond_lvalue_from_string(c-len, len);

				if (rp.data.cond_lval == LI_COMP_UNKNOWN) {
					/* parse error */
					rewrite_parts_free(parts);
					return NULL;
				}

				if (len && *c == '}') {
					rp.type = encoded ? REWRITE_PART_VAR_ENCODED : REWRITE_PART_VAR;
					g_array_append_val(parts, rp);
					c++;
				} else {
					/* parse error */
					rewrite_parts_free(parts);
					return NULL;
				}
			} else {
				/* parse error */
				rewrite_parts_free(parts);
				return NULL;
			}
		} else {
			/* string */
			gchar *first = c;
			c++;

			for (;;) {
				if (*c == '\0' || *c == '?' || *c == '$' || *c == '%') {
					break;
				} else if (*c == '\\') {
					c++;
					if (*c == '\\' || *c == '?' || *c == '$' || *c == '%') {
						c++;
					} else {
						/* parse error */
						rewrite_parts_free(parts);
						return NULL;
					}
				} else {
					c++;
				}
			}

			rp.type = REWRITE_PART_STRING;
			rp.data.str= g_string_new_len(first, c - first);
			g_array_append_val(parts, rp);
		}
	}

	return parts;
}

static gboolean rewrite_internal(liVRequest *vr, GString *dest_path, GString *dest_query, GRegex *regex, GArray *parts, gboolean raw) {
	guint i;
	GString *str;
	GString str_stack;
	gchar *path;
	gint start_pos, end_pos;
	gboolean encoded;
	GString *dest = dest_path;
	GMatchInfo *match_info = NULL;

	if (raw)
		path = vr->request.uri.raw_path->str;
	else
		path = vr->request.uri.path->str;

	if (regex && !g_regex_match(regex, path, 0, &match_info)) {
		if (match_info)
			g_match_info_free(match_info);

		return FALSE;
	}

	g_string_truncate(dest_path, 0);
	g_string_truncate(dest_query, 0);

	if (!parts->len) {
		if (match_info)
			g_match_info_free(match_info);

		return TRUE;
	}

	for (i = 0; i < parts->len; i++) {
		rewrite_part *rp = &g_array_index(parts, rewrite_part, i);
		encoded = FALSE;

		switch (rp->type) {
		case REWRITE_PART_STRING: g_string_append_len(dest, GSTR_LEN(rp->data.str)); break;
		case REWRITE_PART_CAPTURED:
			if (regex && g_match_info_fetch_pos(match_info, rp->data.ndx, &start_pos, &end_pos) && start_pos != -1)
				g_string_append_len(dest, path + start_pos, end_pos - start_pos);

			break;
		case REWRITE_PART_CAPTURED_PREV:
			if (vr->action_stack.regex_stack->len) {
				GArray *rs = vr->action_stack.regex_stack;
				liActionRegexStackElement *arse = &g_array_index(rs, liActionRegexStackElement, rs->len - 1);

				if (arse->string && g_match_info_fetch_pos(arse->match_info, rp->data.ndx, &start_pos, &end_pos) && start_pos != -1)
					g_string_append_len(dest, arse->string->str + start_pos, end_pos - start_pos);
			}
			break;
		case REWRITE_PART_VAR_ENCODED:
			encoded = TRUE;
			/* fall through */
		case REWRITE_PART_VAR:

			switch (rp->data.cond_lval) {
			case LI_COMP_REQUEST_LOCALIP: str = vr->coninfo->local_addr_str; break;
			case LI_COMP_REQUEST_REMOTEIP: str = vr->coninfo->remote_addr_str; break;
			case LI_COMP_REQUEST_SCHEME:
				if (vr->coninfo->is_ssl)
					str_stack = li_const_gstring(CONST_STR_LEN("https"));
				else
					str_stack = li_const_gstring(CONST_STR_LEN("http"));
				str = &str_stack;
				break;
			case LI_COMP_REQUEST_PATH: str = vr->request.uri.path; break;
			case LI_COMP_REQUEST_HOST: str = vr->request.uri.host; break;
			case LI_COMP_REQUEST_QUERY_STRING: str = vr->request.uri.query; break;
			case LI_COMP_REQUEST_METHOD: str = vr->request.http_method_str; break;
			case LI_COMP_REQUEST_CONTENT_LENGTH:
				g_string_printf(vr->wrk->tmp_str, "%"L_GOFFSET_FORMAT, vr->request.content_length);
				str = vr->wrk->tmp_str;
				break;
			default: continue;
			}

			if (encoded)
				li_string_encode_append(str->str, dest, LI_ENCODING_URI);
			else
				g_string_append_len(dest, GSTR_LEN(str));

			break;
		case REWRITE_PART_QUERYSTRING:
			dest = dest_query;
			break;
		}
	}

	g_match_info_free(match_info);

	return TRUE;
}

static liHandlerResult rewrite(liVRequest *vr, gpointer param, gpointer *context) {
	guint i;
	rewrite_rule *rule;
	rewrite_data *rd = param;
	rewrite_plugin_data *rpd = rd->p->data;
	gboolean debug = _OPTION(vr, rd->p, 0).boolean;

	UNUSED(context);

	for (i = 0; i < rd->rules->len; i++) {
		rule = &g_array_index(rd->rules, rewrite_rule, i);

		if (rewrite_internal(vr, vr->wrk->tmp_str, g_ptr_array_index(rpd->tmp_strings, vr->wrk->ndx), rule->regex, rule->parts, FALSE)) {
			/* regex matched */
			if (debug) {
				VR_DEBUG(vr, "rewrite: path \"%s\" => \"%s\", query \"%s\" => \"%s\"",
					vr->request.uri.path->str, vr->wrk->tmp_str->str,
					vr->request.uri.query->str, ((GString*)g_ptr_array_index(rpd->tmp_strings, vr->wrk->ndx))->str
				);
			}

			/* change request path */
			g_string_truncate(vr->request.uri.path, 0);
			g_string_append_len(vr->request.uri.path, GSTR_LEN(vr->wrk->tmp_str));

			/* change request query */
			if (rule->has_querystring) {
				g_string_truncate(vr->request.uri.query, 0);
				g_string_append_len(vr->request.uri.query, GSTR_LEN((GString*)g_ptr_array_index(rpd->tmp_strings, vr->wrk->ndx)));
			}

			/* stop at first matching regex */
			return LI_HANDLER_GO_ON;
		}
	}

	return LI_HANDLER_GO_ON;
}

static void rewrite_free(liServer *srv, gpointer param) {
	guint i;
	rewrite_data *rd = param;

	UNUSED(srv);

	for (i = 0; i < rd->rules->len; i++) {
		rewrite_rule *rule = &g_array_index(rd->rules, rewrite_rule, i);

		rewrite_parts_free(rule->parts);

		if (rule->regex)
			g_regex_unref(rule->regex);
	}

	g_array_free(rd->rules, TRUE);
	g_slice_free(rewrite_data, rd);
}

static liAction* rewrite_create(liServer *srv, liWorker *wrk, liPlugin* p, liValue *val, gpointer userdata) {
	GArray *arr;
	liValue *v;
	guint i;
	rewrite_data *rd;
	rewrite_rule rule;
	rewrite_plugin_data *rpd = p->data;
	GError *err = NULL;

	UNUSED(wrk);
	UNUSED(userdata);

	if (!val || !(val->type == LI_VALUE_STRING || val->type == LI_VALUE_LIST)) {
		ERROR(srv, "%s", "rewrite expects a either a string, a tuple of strings or a list of string tuples");
		return NULL;
	}

	if (!rpd->tmp_strings->len) {
		guint wc = srv->worker_count ? srv->worker_count : 1;
		for (i = 0; i < wc; i++)
			g_ptr_array_add(rpd->tmp_strings, g_string_sized_new(31));
	}

	rd = g_slice_new(rewrite_data);
	rd->p = p;
	rd->rules = g_array_new(FALSE, FALSE, sizeof(rewrite_rule));

	arr = val->data.list;

	if (val->type == LI_VALUE_STRING) {
		/* rewrite "/foo/bar"; */
		rule.has_querystring = FALSE;
		rule.parts = rewrite_parts_parse(val->data.string, &rule.has_querystring);
		rule.regex = NULL;

		if (!rule.parts) {
			rewrite_free(NULL, rd);
			ERROR(srv, "rewrite: error parsing rule \"%s\"", val->data.string->str);
			return NULL;
		}

		g_array_append_val(rd->rules, rule);
	} else if (arr->len == 2 && g_array_index(arr, liValue*, 0)->type == LI_VALUE_STRING && g_array_index(arr, liValue*, 1)->type == LI_VALUE_STRING) {
		/* only one rule */
		rule.has_querystring = FALSE;
		rule.parts = rewrite_parts_parse(g_array_index(arr, liValue*, 1)->data.string, &rule.has_querystring);

		if (!rule.parts) {
			rewrite_free(NULL, rd);
			ERROR(srv, "rewrite: error parsing rule \"%s\"", g_array_index(arr, liValue*, 1)->data.string->str);
			return NULL;
		}

		rule.regex = g_regex_new(g_array_index(arr, liValue*, 0)->data.string->str, G_REGEX_RAW | G_REGEX_OPTIMIZE, 0, &err);

		if (!rule.regex || err) {
			rewrite_free(NULL, rd);
			rewrite_parts_free(rule.parts);
			ERROR(srv, "rewrite: error compiling regex \"%s\": %s", g_array_index(arr, liValue*, 0)->data.string->str, err->message);
			g_error_free(err);
			return NULL;
		}

		g_array_append_val(rd->rules, rule);
	} else {
		/* probably multiple rules */
		for (i = 0; i < arr->len; i++) {
			v = g_array_index(arr, liValue*, i);

			if (v->type != LI_VALUE_LIST || v->data.list->len != 2 ||
				g_array_index(v->data.list, liValue*, 0)->type != LI_VALUE_STRING || g_array_index(v->data.list, liValue*, 1)->type != LI_VALUE_STRING) {

				rewrite_free(NULL, rd);
				ERROR(srv, "%s", "rewrite expects a either a tuple of strings or a list of those");
				return NULL;
			}

			rule.has_querystring = FALSE;
			rule.parts = rewrite_parts_parse(g_array_index(v->data.list, liValue*, 1)->data.string, &rule.has_querystring);

			if (!rule.parts) {
				rewrite_free(NULL, rd);
				ERROR(srv, "rewrite: error parsing rule \"%s\"", g_array_index(v->data.list, liValue*, 1)->data.string->str);
				return NULL;
			}

			rule.regex = g_regex_new(g_array_index(v->data.list, liValue*, 0)->data.string->str, G_REGEX_RAW | G_REGEX_OPTIMIZE, 0, &err);

			if (!rule.regex || err) {
				rewrite_free(NULL, rd);
				rewrite_parts_free(rule.parts);
				ERROR(srv, "rewrite: error compiling regex \"%s\": %s", g_array_index(v->data.list, liValue*, 0)->data.string->str, err->message);
				g_error_free(err);
				return NULL;
			}

			g_array_append_val(rd->rules, rule);
		}
	}


	return li_action_new_function(rewrite, NULL, rewrite_free, rd);
}



static const liPluginOption options[] = {
	{ "rewrite.debug", LI_VALUE_BOOLEAN, FALSE, NULL },

	{ NULL, 0, 0, NULL }
};

static const liPluginAction actions[] = {
	{ "rewrite", rewrite_create, NULL },

	{ NULL, NULL, NULL }
};

static const liPluginSetup setups[] = {
	{ NULL, NULL, NULL }
};


static void plugin_rewrite_free(liServer *srv, liPlugin *p) {
	guint i;
	rewrite_plugin_data *data = p->data;

	UNUSED(srv);

	for (i = 0; i < data->tmp_strings->len; i++)
		g_string_free(g_ptr_array_index(data->tmp_strings, i), TRUE);

	g_ptr_array_free(data->tmp_strings, TRUE);
	g_slice_free(rewrite_plugin_data, data);
}

static void plugin_rewrite_init(liServer *srv, liPlugin *p, gpointer userdata) {
	UNUSED(srv); UNUSED(userdata);

	p->options = options;
	p->actions = actions;
	p->setups = setups;

	p->free = plugin_rewrite_free;
	
	p->data = g_slice_new(rewrite_plugin_data);
	((rewrite_plugin_data*)p->data)->tmp_strings = g_ptr_array_new();
}


gboolean mod_rewrite_init(liModules *mods, liModule *mod) {
	UNUSED(mod);

	MODULE_VERSION_CHECK(mods);

	mod->config = li_plugin_register(mods->main, "mod_rewrite", plugin_rewrite_init, NULL);

	return mod->config != NULL;
}

gboolean mod_rewrite_free(liModules *mods, liModule *mod) {
	if (mod->config)
		li_plugin_free(mods->main, mod->config);

	return TRUE;
}

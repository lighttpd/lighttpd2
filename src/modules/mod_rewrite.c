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
 * Authors:
 *     Copyright (c) 2009 Thomas Porzelt
 *     Copyright (c) 2010 Stefan Bühler
 * License:
 *     MIT, see COPYING file in the lighttpd 2 tree
 */

#include <lighttpd/base.h>
#include <lighttpd/encoding.h>
#include <lighttpd/pattern.h>

LI_API gboolean mod_rewrite_init(liModules *mods, liModule *mod);
LI_API gboolean mod_rewrite_free(liModules *mods, liModule *mod);

typedef struct rewrite_plugin_data rewrite_plugin_data;
struct rewrite_plugin_data {
	GPtrArray *tmp_strings; /* array of (GString*) */
};

typedef struct rewrite_rule rewrite_rule;
struct rewrite_rule {
	liPattern *path, *querystring;
	GRegex *regex;
};

typedef struct rewrite_data rewrite_data;
struct rewrite_data {
	GArray *rules;
	liPlugin *p;
	gboolean raw;
};

static gboolean rewrite_rule_parse(liServer *srv, GString *regex, GString *str, rewrite_rule *rule) {
	gchar *qs;

	rule->path = rule->querystring = NULL;
	rule->regex = NULL;

	/* find "not-escaped" ? */
	for (qs = str->str; *qs; qs++) {
		if ('\\' == *qs) {
			qs++;
			if (!*qs) break;
		} else if ('?' == *qs) break;
	}
	if (!*qs) qs = NULL;

	if (NULL != qs) {
		*qs = '\0'; /* restore later */
		rule->querystring = li_pattern_new(srv, qs+1);
		if (NULL == rule->querystring) {
			goto error;
		}
	}

	rule->path = li_pattern_new(srv, str->str);
	if (NULL == rule->path) {
		goto error;
	}

	if (NULL != regex) {
		GError *err = NULL;
		rule->regex = g_regex_new(regex->str, G_REGEX_RAW | G_REGEX_OPTIMIZE, 0, &err);

		if (NULL == rule->regex || NULL != err) {
			ERROR(srv, "rewrite: error compiling regex \"%s\": %s", regex->str, NULL != err ? err->message : "unknown error");
			g_error_free(err);
			goto error;
		}
	}

	if (NULL != qs) {
		*qs = '?';
	}

	return TRUE;

error:
	if (NULL != rule->querystring) {
		li_pattern_free(rule->querystring);
		rule->querystring = NULL;
	}
	if (NULL != rule->path) {
		li_pattern_free(rule->path);
		rule->path = NULL;
	}
	if (NULL != rule->regex) {
		g_regex_unref(rule->regex);
		rule->regex = NULL;
	}

	if (NULL != qs) {
		*qs = '?';
	}

	return FALSE;
}

static gboolean rewrite_internal(liVRequest *vr, GString *dest_path, GString *dest_query, rewrite_rule *rule, gboolean raw) {
	gchar *path;
	GMatchInfo *match_info = NULL;
	GMatchInfo *prev_match_info = NULL;

	if (raw) {
		path = vr->request.uri.raw_path->str;
	} else {
		path = vr->request.uri.path->str;
	}

	if (NULL != rule->regex && !g_regex_match(rule->regex, path, 0, &match_info)) {
		if (NULL != match_info) {
			g_match_info_free(match_info);
		}

		return FALSE;
	}

	if (vr->action_stack.regex_stack->len) {
		GArray *rs = vr->action_stack.regex_stack;
		prev_match_info = g_array_index(rs, liActionRegexStackElement, rs->len - 1).match_info;
	}

	g_string_truncate(dest_path, 0);
	g_string_truncate(dest_query, 0);

	li_pattern_eval(vr, dest_path, rule->path, li_pattern_regex_cb, match_info, li_pattern_regex_cb, prev_match_info);
	if (NULL != rule->querystring) {
		li_pattern_eval(vr, dest_query, rule->querystring, li_pattern_regex_cb, match_info, li_pattern_regex_cb, prev_match_info);
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
		GString *dest_path = vr->wrk->tmp_str;
		GString *dest_query = g_ptr_array_index(rpd->tmp_strings, vr->wrk->ndx);

		rule = &g_array_index(rd->rules, rewrite_rule, i);

		if (rewrite_internal(vr, dest_path, dest_query, rule, rd->raw)) {
			/* regex matched */
			if (debug) {
				VR_DEBUG(vr, "rewrite: path \"%s\" => \"%s\", query \"%s\" => \"%s\"",
					vr->request.uri.path->str, dest_path->str,
					vr->request.uri.query->str, dest_query->str
				);
			}

			/* change request path */
			g_string_truncate(vr->request.uri.path, 0);
			g_string_append_len(vr->request.uri.path, GSTR_LEN(dest_path));

			/* change request query */
			if (NULL != rule->querystring) {
				g_string_truncate(vr->request.uri.query, 0);
				g_string_append_len(vr->request.uri.query, GSTR_LEN(dest_query));
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

		li_pattern_free(rule->path);
		li_pattern_free(rule->querystring);

		if (rule->regex) {
			g_regex_unref(rule->regex);
		}
	}

	g_array_free(rd->rules, TRUE);
	g_slice_free(rewrite_data, rd);
}

static liAction* rewrite_create(liServer *srv, liWorker *wrk, liPlugin* p, liValue *val, gpointer userdata) {
	GArray *arr;
	liValue *v;
	guint i;
	rewrite_data *rd;
	rewrite_plugin_data *rpd = p->data;

	UNUSED(wrk);

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
	rd->raw = GPOINTER_TO_INT(userdata);

	arr = val->data.list;

	if (val->type == LI_VALUE_STRING) {
		/* rewrite "/foo/bar"; */
		rewrite_rule rule = { NULL, NULL, NULL };

		if (!rewrite_rule_parse(srv, NULL, val->data.string, &rule)) {
			rewrite_free(NULL, rd);
			ERROR(srv, "rewrite: error parsing rule \"%s\"", val->data.string->str);
			return NULL;
		}

		g_array_append_val(rd->rules, rule);
	} else if (arr->len == 2 && g_array_index(arr, liValue*, 0)->type == LI_VALUE_STRING && g_array_index(arr, liValue*, 1)->type == LI_VALUE_STRING) {
		/* only one rule */
		rewrite_rule rule = { NULL, NULL, NULL };

		if (!rewrite_rule_parse(srv, g_array_index(arr, liValue*, 0)->data.string, g_array_index(arr, liValue*, 1)->data.string, &rule)) {
			rewrite_free(NULL, rd);
			return NULL;
		}

		g_array_append_val(rd->rules, rule);
	} else {
		/* probably multiple rules */
		for (i = 0; i < arr->len; i++) {
			rewrite_rule rule = { NULL, NULL, NULL };
			v = g_array_index(arr, liValue*, i);

			if (v->type != LI_VALUE_LIST || v->data.list->len != 2 ||
				g_array_index(v->data.list, liValue*, 0)->type != LI_VALUE_STRING || g_array_index(v->data.list, liValue*, 1)->type != LI_VALUE_STRING) {

				rewrite_free(NULL, rd);
				ERROR(srv, "%s", "rewrite expects a either a tuple of strings or a list of those");
				return NULL;
			}


			if (!rewrite_rule_parse(srv, g_array_index(v->data.list, liValue*, 0)->data.string, g_array_index(v->data.list, liValue*, 1)->data.string, &rule)) {
				rewrite_free(NULL, rd);
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
	{ "rewrite", rewrite_create, GINT_TO_POINTER(FALSE) },
	{ "rewrite_raw", rewrite_create, GINT_TO_POINTER(TRUE) },

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

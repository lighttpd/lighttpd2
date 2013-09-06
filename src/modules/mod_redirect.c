/*
 * mod_redirect - redirect clients by sending a http status code 301 plus Location header
 *
 * Description:
 *     mod_redirect acts similar to mod_rewrite but redirects clients instead of rewriting the request.
 *     It supports matching regular expressions and substitution with captured substrings as well as other placeholders.
 *     A so called redirect rule consist of a regular expression and a target string.
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
 *     redirect.debug = <true|false>;
 *         - if set, debug information is written to the log
 * Actions:
 *     redirect "http://example.tld/";
 *         - redirects the client, substituting all placeholders. $0..$9 get replaced by empty strings.
 *     redirect "regex" => "/new/path";
 *         - redirects client if "regex" matched the request.path.
 *         - $0..$9 get replaced by the captured substrings of the regular expression "regex".
 *     redirect ("regex1" => "/new/path1", ..., "regexN" => "/new/pathN");
 *         - traverses the list of redirect rules.
 *         - redirects client to the corresponding "/new/path" if the regex matches and stops traversing the list.
 *
 * Example config:
 *     # redirect all non www. requests. for example: foo.tld/bar?x=y to www.foo.tld/bar?x=y
 *     if request.host !~ "^www\.(.*)$" {
 *         redirect "." => "http://www.%1/$0?%{request.query}";
 *     }
 *
 *
 * Tip:
 *     As both config parser and regex compiler use backslashes to escape special characters, you will have to escape them twice.
 *     For example "^foo\\dbar$" will end up as "^foo\dbar$" as regex input, which would match things like "foo3bar".
 *
 * Todo:
 *     none
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

LI_API gboolean mod_redirect_init(liModules *mods, liModule *mod);
LI_API gboolean mod_redirect_free(liModules *mods, liModule *mod);

typedef struct redirect_rule redirect_rule;
struct redirect_rule {
	liPattern *pattern;
	GRegex *regex;
	enum {
		REDIRECT_ABSOLUTE_URI,
		REDIRECT_ABSOLUTE_PATH,
		REDIRECT_RELATIVE_PATH,
		REDIRECT_RELATIVE_QUERY
	} type;
};

typedef struct redirect_data redirect_data;
struct redirect_data {
	GArray *rules;
	liPlugin *p;
};

static gboolean redirect_rule_parse(liServer *srv, GString *regex, GString *str, redirect_rule *rule) {
	gchar *pattern_str = str->str;

	rule->pattern = NULL;
	rule->regex = NULL;
	rule->type = REDIRECT_ABSOLUTE_URI;

	if (pattern_str[0] == '/') {
		rule->type = REDIRECT_ABSOLUTE_PATH;
	} else if (pattern_str[0] == '?') {
		rule->type = REDIRECT_RELATIVE_QUERY;
	} else if (g_str_has_prefix(pattern_str, "./")) {
		pattern_str += 2;
		rule->type = REDIRECT_RELATIVE_PATH;
	}

	rule->pattern = li_pattern_new(srv, pattern_str);
	if (NULL == rule->pattern) {
		goto error;
	}

	if (NULL != regex) {
		GError *err = NULL;
		rule->regex = g_regex_new(regex->str, G_REGEX_RAW | G_REGEX_OPTIMIZE, 0, &err);

		if (NULL == rule->regex || NULL != err) {
			ERROR(srv, "redirect: error compiling regex \"%s\": %s", regex->str, NULL != err ? err->message : "unknown error");
			g_error_free(err);
			goto error;
		}
	}

	return TRUE;

error:
	if (NULL != rule->pattern) {
		li_pattern_free(rule->pattern);
		rule->pattern = NULL;
	}
	if (NULL != rule->regex) {
		g_regex_unref(rule->regex);
		rule->regex = NULL;
	}

	return FALSE;
}

static gboolean redirect_internal(liVRequest *vr, GString *dest, redirect_rule *rule) {
	gchar *path;
	GMatchInfo *match_info = NULL;
	GMatchInfo *prev_match_info = NULL;

	path = vr->request.uri.path->str;

	if (NULL != rule->regex && !g_regex_match(rule->regex, path, 0, &match_info)) {
		if (match_info) {
			g_match_info_free(match_info);
		}

		return FALSE;
	}

	if (vr->action_stack.regex_stack->len) {
		GArray *rs = vr->action_stack.regex_stack;
		prev_match_info = g_array_index(rs, liActionRegexStackElement, rs->len - 1).match_info;
	}

	g_string_truncate(dest, 0);

	switch (rule->type) {
	case REDIRECT_ABSOLUTE_URI:
		/* http://example.tld/foo/bar?baz */
		break;
	case REDIRECT_ABSOLUTE_PATH:
		/* /foo/bar?baz */
		g_string_append_len(dest, GSTR_LEN(vr->request.uri.scheme));
		g_string_append_len(dest, CONST_STR_LEN("://"));
		g_string_append_len(dest, GSTR_LEN(vr->request.uri.authority));
		break;
	case REDIRECT_RELATIVE_PATH:
		/* foo/bar?baz */
		g_string_append_len(dest, GSTR_LEN(vr->request.uri.scheme));
		g_string_append_len(dest, CONST_STR_LEN("://"));
		g_string_append_len(dest, GSTR_LEN(vr->request.uri.authority));
		/* search for last slash /foo/bar */
		{
			gchar *c;
			for (c = (vr->request.uri.path->str + vr->request.uri.path->len); c-- > vr->request.uri.path->str;) {
				if (*c == '/') break;
			}

			g_string_append_len(dest, vr->request.uri.path->str, c - vr->request.uri.path->str + 1);
		}
		break;
	case REDIRECT_RELATIVE_QUERY:
		/* ?bar */
		g_string_append_len(dest, GSTR_LEN(vr->request.uri.scheme));
		g_string_append_len(dest, CONST_STR_LEN("://"));
		g_string_append_len(dest, GSTR_LEN(vr->request.uri.authority));
		g_string_append_len(dest, GSTR_LEN(vr->request.uri.path));
		break;
	}

	li_pattern_eval(vr, dest, rule->pattern, li_pattern_regex_cb, match_info, li_pattern_regex_cb, prev_match_info);

	if (match_info) {
		g_match_info_free(match_info);
	}

	return TRUE;
}

static liHandlerResult redirect(liVRequest *vr, gpointer param, gpointer *context) {
	guint i;
	redirect_rule *rule;
	redirect_data *rd = param;
	gboolean debug = _OPTION(vr, rd->p, 0).boolean;
	GString *dest = vr->wrk->tmp_str;

	UNUSED(context);

	if (li_vrequest_is_handled(vr)) return LI_HANDLER_GO_ON;

	for (i = 0; i < rd->rules->len; i++) {
		rule = &g_array_index(rd->rules, redirect_rule, i);

		if (redirect_internal(vr, dest, rule)) {
			/* regex matched */
			if (debug) {
				VR_DEBUG(vr, "redirect: \"%s\"", dest->str);
			}

			if (!li_vrequest_handle_direct(vr)) return LI_HANDLER_ERROR;

			vr->response.http_status = 301;
			li_http_header_overwrite(vr->response.headers, CONST_STR_LEN("Location"), GSTR_LEN(dest));

			/* stop at first matching regex */
			return LI_HANDLER_GO_ON;
		}
	}

	return LI_HANDLER_GO_ON;
}

static void redirect_free(liServer *srv, gpointer param) {
	guint i;
	redirect_data *rd = param;

	UNUSED(srv);

	for (i = 0; i < rd->rules->len; i++) {
		redirect_rule *rule = &g_array_index(rd->rules, redirect_rule, i);

		li_pattern_free(rule->pattern);

		if (NULL != rule->regex)
			g_regex_unref(rule->regex);
	}

	g_array_free(rd->rules, TRUE);
	g_slice_free(redirect_data, rd);
}

static liAction* redirect_create(liServer *srv, liWorker *wrk, liPlugin* p, liValue *val, gpointer userdata) {
	redirect_data *rd;
	UNUSED(wrk); UNUSED(userdata);

	val = li_value_get_single_argument(val);

	if (LI_VALUE_STRING != li_value_type(val) && LI_VALUE_LIST != li_value_type(val)) {
		ERROR(srv, "%s", "redirect expects a either a string, a tuple of strings or a list of string tuples");
		return NULL;
	}

	rd = g_slice_new(redirect_data);
	rd->p = p;
	rd->rules = g_array_new(FALSE, FALSE, sizeof(redirect_rule));

	if (LI_VALUE_STRING == li_value_type(val)) {
		redirect_rule rule;

		/* redirect "/foo/bar"; */
		if (!redirect_rule_parse(srv, NULL, val->data.string, &rule)) {
			redirect_free(NULL, rd);
			return NULL;
		}

		g_array_append_val(rd->rules, rule);
	} else if (li_value_list_has_len(val, 2) && LI_VALUE_STRING == li_value_list_type_at(val, 0) && LI_VALUE_STRING == li_value_list_type_at(val, 1)) {
		redirect_rule rule;

		/* only one rule */
		if (!redirect_rule_parse(srv, li_value_list_at(val, 0)->data.string, li_value_list_at(val, 1)->data.string, &rule)) {
			redirect_free(NULL, rd);
			return NULL;
		}

		g_array_append_val(rd->rules, rule);
	} else {
		/* probably multiple rules */
		LI_VALUE_FOREACH(v, val)
			redirect_rule rule;

			if (!li_value_list_has_len(v, 2)
					|| LI_VALUE_STRING != li_value_list_type_at(v, 0) || LI_VALUE_STRING != li_value_list_type_at(v, 1)) {
				redirect_free(NULL, rd);
				ERROR(srv, "%s", "redirect expects a either a tuple of strings or a list of those");
				return NULL;
			}

			if (!redirect_rule_parse(srv, li_value_list_at(val, 0)->data.string, li_value_list_at(val, 1)->data.string, &rule)) {
				redirect_free(NULL, rd);
				return NULL;
			}

			g_array_append_val(rd->rules, rule);
		LI_VALUE_END_FOREACH()
	}

	return li_action_new_function(redirect, NULL, redirect_free, rd);
}



static const liPluginOption options[] = {
	{ "redirect.debug", LI_VALUE_BOOLEAN, FALSE, NULL },

	{ NULL, 0, 0, NULL }
};

static const liPluginAction actions[] = {
	{ "redirect", redirect_create, NULL },

	{ NULL, NULL, NULL }
};

static const liPluginSetup setups[] = {
	{ NULL, NULL, NULL }
};

static void plugin_redirect_init(liServer *srv, liPlugin *p, gpointer userdata) {
	UNUSED(srv); UNUSED(userdata);

	p->options = options;
	p->actions = actions;
	p->setups = setups;
}


gboolean mod_redirect_init(liModules *mods, liModule *mod) {
	UNUSED(mod);

	MODULE_VERSION_CHECK(mods);

	mod->config = li_plugin_register(mods->main, "mod_redirect", plugin_redirect_init, NULL);

	return mod->config != NULL;
}

gboolean mod_redirect_free(liModules *mods, liModule *mod) {
	if (mod->config)
		li_plugin_free(mods->main, mod->config);

	return TRUE;
}

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
 * Author:
 *     Copyright (c) 2009 Thomas Porzelt
 * License:
 *     MIT, see COPYING file in the lighttpd 2 tree
 */

#include <lighttpd/base.h>
#include <lighttpd/encoding.h>

LI_API gboolean mod_redirect_init(liModules *mods, liModule *mod);
LI_API gboolean mod_redirect_free(liModules *mods, liModule *mod);

struct redirect_part {
	enum {
		REDIRECT_PART_STRING,
		REDIRECT_PART_CAPTURED,
		REDIRECT_PART_CAPTURED_PREV,
		REDIRECT_PART_VAR,
		REDIRECT_PART_VAR_ENCODED
	} type;
	union {
		GString *str;
		guint8 ndx;
		liCondLValue cond_lval;
	} data;
};
typedef struct redirect_part redirect_part;

struct redirect_rule {
	GArray *parts;
	GRegex *regex;
	enum {
		REDIRECT_ABSOLUTE_URI,
		REDIRECT_ABSOLUTE_PATH,
		REDIRECT_RELATIVE_PATH,
		REDIRECT_RELATIVE_QUERY
	} type;
};
typedef struct redirect_rule redirect_rule;

struct redirect_data {
	GArray *rules;
	liPlugin *p;
};
typedef struct redirect_data redirect_data;



static void redirect_parts_free(GArray *parts) {
	guint i;

	for (i = 0; i < parts->len; i++) {
		redirect_part *rp = &g_array_index(parts, redirect_part, i);

		switch (rp->type) {
		case REDIRECT_PART_STRING: g_string_free(rp->data.str, TRUE); break;
		default: break;
		}
	}

	g_array_free(parts, TRUE);
}

static GArray *redirect_parts_parse(GString *str) {
	redirect_part rp;
	gboolean encoded;
	gchar *c = str->str;
	GArray *parts = g_array_new(FALSE, FALSE, sizeof(redirect_part));

	for (;;) {
		if (*c == '\0') {
			break;
		} else if (*c == '$') {
			c++;
			if (*c >= '0' && *c <= '9') {
				/* $n backreference */
				rp.type = REDIRECT_PART_CAPTURED;
				rp.data.ndx = *c - '0';
				g_array_append_val(parts, rp);
				c++;
			} else {
				/* parse error */
				redirect_parts_free(parts);
				return NULL;
			}
		} else if (*c == '%') {
			c++;
			if (*c >= '0' && *c <= '9') {
				/* %n backreference */
				rp.type = REDIRECT_PART_CAPTURED_PREV;
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
					redirect_parts_free(parts);
					return NULL;
				}

				rp.data.cond_lval = li_cond_lvalue_from_string(c-len, len);

				if (rp.data.cond_lval == LI_COMP_UNKNOWN) {
					/* parse error */
					redirect_parts_free(parts);
					return NULL;
				}

				if (len && *c == '}') {
					rp.type = encoded ? REDIRECT_PART_VAR_ENCODED : REDIRECT_PART_VAR;
					g_array_append_val(parts, rp);
					c++;
				} else {
					/* parse error */
					redirect_parts_free(parts);
					return NULL;
				}
			} else {
				/* parse error */
				redirect_parts_free(parts);
				return NULL;
			}
		} else {
			/* string */
			gchar *first = c;
			c++;

			for (;;) {
				if (*c == '\0' || *c == '$' || *c == '%') {
					break;
				} else if (*c == '\\') {
					c++;
					if (*c == '\\' || *c == '$' || *c == '%') {
						c++;
					} else {
						/* parse error */
						redirect_parts_free(parts);
						return NULL;
					}
				} else {
					c++;
				}
			}

			rp.type = REDIRECT_PART_STRING;
			rp.data.str= g_string_new_len(first, c - first);
			g_array_append_val(parts, rp);
		}
	}

	return parts;
}

static gboolean redirect_internal(liVRequest *vr, GString *dest, redirect_rule *rule, gboolean raw) {
	guint i;
	GString *str;
	gchar *path;
	gchar *c;
	gint start_pos, end_pos;
	gboolean encoded;
	GRegex *regex = rule->regex;
	GArray *parts = rule->parts;
	GMatchInfo *match_info = NULL;

	if (raw)
		path = vr->request.uri.raw->str;
	else
		path = vr->request.uri.path->str;

	if (regex && !g_regex_match(regex, path, 0, &match_info)) {
		if (match_info)
			g_match_info_free(match_info);

		return FALSE;
	}

	g_string_truncate(dest, 0);

	if (!parts->len || g_array_index(parts, redirect_part, 0).type != REDIRECT_PART_VAR || g_array_index(parts, redirect_part, 0).data.cond_lval != LI_COMP_REQUEST_SCHEME) {
		switch (rule->type) {
		case REDIRECT_ABSOLUTE_URI:
			/* http://example.tld/foo/bar?baz */
			break;
		case REDIRECT_ABSOLUTE_PATH:
			/* /foo/bar?baz */
			g_string_append_len(dest, CONST_STR_LEN(vr->con->is_ssl ? "https" : "http"));
			g_string_append_len(dest, CONST_STR_LEN("://"));
			g_string_append_len(dest, GSTR_LEN(vr->request.uri.authority));
			break;
		case REDIRECT_RELATIVE_PATH:
			/* foo/bar?baz */
			g_string_append_len(dest, CONST_STR_LEN(vr->con->is_ssl ? "https" : "http"));
			g_string_append_len(dest, CONST_STR_LEN("://"));
			g_string_append_len(dest, GSTR_LEN(vr->request.uri.authority));
			/* search for last slash /foo/bar */
			for (c = (vr->request.uri.path->str + vr->request.uri.path->len - 1); c > vr->request.uri.path->str; c--) {
				if (*c == '/')
					break;
			}

			g_string_append_len(dest, vr->request.uri.path->str, c - vr->request.uri.path->str + 1);
			break;
		case REDIRECT_RELATIVE_QUERY:
			/* ?bar */
			g_string_append_len(dest, CONST_STR_LEN(vr->con->is_ssl ? "https" : "http"));
			g_string_append_len(dest, CONST_STR_LEN("://"));
			g_string_append_len(dest, GSTR_LEN(vr->request.uri.authority));
			g_string_append_len(dest, GSTR_LEN(vr->request.uri.path));
			break;
		}
	}

	for (i = 0; i < parts->len; i++) {
		redirect_part *rp = &g_array_index(parts, redirect_part, i);
		encoded = FALSE;

		switch (rp->type) {
		case REDIRECT_PART_STRING: g_string_append_len(dest, GSTR_LEN(rp->data.str)); break;
		case REDIRECT_PART_CAPTURED:
			if (regex && g_match_info_fetch_pos(match_info, rp->data.ndx, &start_pos, &end_pos) && start_pos != -1)
				g_string_append_len(dest, path + start_pos, end_pos - start_pos);

			break;
		case REDIRECT_PART_CAPTURED_PREV:
			if (vr->action_stack.regex_stack->len) {
				GArray *rs = vr->action_stack.regex_stack;
				liActionRegexStackElement *arse = &g_array_index(rs, liActionRegexStackElement, rs->len - 1);

				if (arse->string && g_match_info_fetch_pos(arse->match_info, rp->data.ndx, &start_pos, &end_pos) && start_pos != -1)
					g_string_append_len(dest, arse->string->str + start_pos, end_pos - start_pos);
			}
			break;
		case REDIRECT_PART_VAR_ENCODED:
			encoded = TRUE;
			/* fall through */
		case REDIRECT_PART_VAR:

			switch (rp->data.cond_lval) {
			case LI_COMP_REQUEST_LOCALIP: str = vr->con->srv_sock->local_addr_str; break;
			case LI_COMP_REQUEST_REMOTEIP: str = vr->con->remote_addr_str; break;
			case LI_COMP_REQUEST_SCHEME: str = NULL; break;
			case LI_COMP_REQUEST_PATH: str = vr->request.uri.path; break;
			case LI_COMP_REQUEST_HOST: str = vr->request.uri.host; break;
			case LI_COMP_REQUEST_QUERY_STRING: str = vr->request.uri.query; break;
			case LI_COMP_REQUEST_METHOD: str = vr->request.http_method_str; break;
			case LI_COMP_REQUEST_CONTENT_LENGTH:
				g_string_printf(vr->con->wrk->tmp_str, "%"L_GOFFSET_FORMAT, vr->request.content_length);
				str = vr->con->wrk->tmp_str;
				break;
			default: continue;
			}

			if (rp->data.cond_lval == LI_COMP_REQUEST_SCHEME) {
				if (vr->con->is_ssl)
					g_string_append_len(dest, CONST_STR_LEN("https"));
				else
					g_string_append_len(dest, CONST_STR_LEN("http"));
			} else {
				if (encoded)
					li_string_encode_append(str->str, dest, LI_ENCODING_URI);
				else
					g_string_append_len(dest, GSTR_LEN(str));
			}

			break;
		}
	}

	if (match_info)
		g_match_info_free(match_info);

	return TRUE;
}

static liHandlerResult redirect(liVRequest *vr, gpointer param, gpointer *context) {
	guint i;
	redirect_rule *rule;
	redirect_data *rd = param;
	gboolean debug = _OPTION(vr, rd->p, 0).boolean;
	GString *dest = vr->wrk->tmp_str;

	UNUSED(context);

	for (i = 0; i < rd->rules->len; i++) {
		rule = &g_array_index(rd->rules, redirect_rule, i);

		if (redirect_internal(vr, dest, rule, FALSE)) {
			/* regex matched */
			if (debug)
				VR_DEBUG(vr, "redirect: \"%s\"", dest->str);

			if (!li_vrequest_handle_direct(vr))
				return LI_HANDLER_ERROR;

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

		redirect_parts_free(rule->parts);

		if (rule->regex)
			g_regex_unref(rule->regex);
	}

	g_array_free(rd->rules, TRUE);
	g_slice_free(redirect_data, rd);
}

static liAction* redirect_create(liServer *srv, liPlugin* p, liValue *val) {
	GArray *arr;
	liValue *v;
	guint i;
	redirect_data *rd;
	redirect_rule rule;
	GError *err = NULL;

	UNUSED(srv);
	UNUSED(p);

	if (!val || !(val->type == LI_VALUE_STRING || val->type == LI_VALUE_LIST)) {
		ERROR(srv, "%s", "redirect expects a either a string, a tuple of strings or a list of string tuples");
		return NULL;
	}

	rd = g_slice_new(redirect_data);
	rd->p = p;
	rd->rules = g_array_new(FALSE, FALSE, sizeof(redirect_rule));

	arr = val->data.list;

	if (val->type == LI_VALUE_STRING) {
		/* redirect "/foo/bar"; */
		if (g_str_has_prefix(val->data.string->str, "http://"))
			rule.type = REDIRECT_ABSOLUTE_URI;
		else if (val->data.string->str[0] == '/')
			rule.type = REDIRECT_ABSOLUTE_PATH;
		else if (val->data.string->str[0] == '?')
			rule.type = REDIRECT_RELATIVE_QUERY;
		else
			rule.type = REDIRECT_RELATIVE_PATH;

		rule.parts = redirect_parts_parse(val->data.string);
		rule.regex = NULL;

		if (!rule.parts) {
			redirect_free(NULL, rd);
			ERROR(srv, "redirect: error parsing rule \"%s\"", val->data.string->str);
			return NULL;
		}

		g_array_append_val(rd->rules, rule);
	} else if (arr->len == 2 && g_array_index(arr, liValue*, 0)->type == LI_VALUE_STRING && g_array_index(arr, liValue*, 1)->type == LI_VALUE_STRING) {
		/* only one rule */
		if (g_str_has_prefix(g_array_index(arr, liValue*, 1)->data.string->str, "http://"))
			rule.type = REDIRECT_ABSOLUTE_URI;
		else if (g_array_index(arr, liValue*, 1)->data.string->str[0] == '/')
			rule.type = REDIRECT_ABSOLUTE_PATH;
		else if (g_array_index(arr, liValue*, 1)->data.string->str[0] == '?')
			rule.type = REDIRECT_RELATIVE_QUERY;
		else
			rule.type = REDIRECT_RELATIVE_PATH;

		rule.parts = redirect_parts_parse(g_array_index(arr, liValue*, 1)->data.string);

		if (!rule.parts) {
			redirect_free(NULL, rd);
			ERROR(srv, "redirect: error parsing rule \"%s\"", g_array_index(arr, liValue*, 1)->data.string->str);
			return NULL;
		}

		rule.regex = g_regex_new(g_array_index(arr, liValue*, 0)->data.string->str, G_REGEX_RAW | G_REGEX_OPTIMIZE, 0, &err);

		if (!rule.regex || err) {
			redirect_free(NULL, rd);
			redirect_parts_free(rule.parts);
			ERROR(srv, "redirect: error compiling regex \"%s\": %s", g_array_index(arr, liValue*, 0)->data.string->str, err->message);
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

				redirect_free(NULL, rd);
				ERROR(srv, "%s", "redirect expects a either a tuple of strings or a list of those");
				return NULL;
			}

			if (g_str_has_prefix(g_array_index(v->data.list, liValue*, 1)->data.string->str, "http://"))
				rule.type = REDIRECT_ABSOLUTE_URI;
			else if (g_array_index(v->data.list, liValue*, 1)->data.string->str[0] == '/')
				rule.type = REDIRECT_ABSOLUTE_PATH;
			else if (g_array_index(v->data.list, liValue*, 1)->data.string->str[0] == '?')
				rule.type = REDIRECT_RELATIVE_QUERY;
			else
				rule.type = REDIRECT_RELATIVE_PATH;

			rule.parts = redirect_parts_parse(g_array_index(v->data.list, liValue*, 1)->data.string);

			if (!rule.parts) {
				redirect_free(NULL, rd);
				ERROR(srv, "redirect: error parsing rule \"%s\"", g_array_index(v->data.list, liValue*, 1)->data.string->str);
				return NULL;
			}

			rule.regex = g_regex_new(g_array_index(v->data.list, liValue*, 0)->data.string->str, G_REGEX_RAW | G_REGEX_OPTIMIZE, 0, &err);

			if (!rule.regex || err) {
				redirect_free(NULL, rd);
				redirect_parts_free(rule.parts);
				ERROR(srv, "redirect: error compiling regex \"%s\": %s", g_array_index(v->data.list, liValue*, 0)->data.string->str, err->message);
				g_error_free(err);
				return NULL;
			}

			g_array_append_val(rd->rules, rule);
		}
	}


	return li_action_new_function(redirect, NULL, redirect_free, rd);
}



static const liPluginOption options[] = {
	{ "redirect.debug", LI_VALUE_BOOLEAN, NULL, NULL, NULL },

	{ NULL, 0, NULL, NULL, NULL }
};

static const liPluginAction actions[] = {
	{ "redirect", redirect_create },

	{ NULL, NULL }
};

static const liPluginSetup setups[] = {
	{ NULL, NULL }
};

static void plugin_redirect_init(liServer *srv, liPlugin *p) {
	UNUSED(srv);

	p->options = options;
	p->actions = actions;
	p->setups = setups;
}


gboolean mod_redirect_init(liModules *mods, liModule *mod) {
	UNUSED(mod);

	MODULE_VERSION_CHECK(mods);

	mod->config = li_plugin_register(mods->main, "mod_redirect", plugin_redirect_init);

	return mod->config != NULL;
}

gboolean mod_redirect_free(liModules *mods, liModule *mod) {
	if (mod->config)
		li_plugin_free(mods->main, mod->config);

	return TRUE;
}

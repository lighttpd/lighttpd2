/*
 * mod_userdir - user-specific document roots
 *
 * Description:
 *     mod_userdir allows you to have user-specific document roots being accessed through http://domain/~user/
 *
 * Setups:
 *     none
 *
 * Options:
 *     none
 *
 * Actions:
 *     userdir <path>;
 *         - if not starting with a slash, maps a request path of /~user/ to a docroot of ~user/<path>/
 *         - if starting with a slash, maps a request path of /~user/ to a docroot of <path>
 *         - * in <path> is replaced by the requested username
 *         - $1-9 are replace by the n-th letter of the requested username
 *
 * Example config:
 *     userdir "public_html"; # maps /~lighty/ to ~lighty/public_html/ (e.g. /home/lighty/public_html/ on most systems)
 *
 * Todo:
 *     - userdir.exclude / userdir.include options/setups to allow certain users to be excluded or included
 *
 * Author:
 *     Copyright (c) 2010 Thomas Porzelt
 * License:
 *     MIT, see COPYING file in the lighttpd 2 tree
 */

#include <lighttpd/base.h>
#include <pwd.h>

LI_API gboolean mod_userdir_init(liModules *mods, liModule *mod);
LI_API gboolean mod_userdir_free(liModules *mods, liModule *mod);

struct userdir_part {
	enum {
		USERDIR_PART_STRING,
		USERDIR_PART_USERNAME,
		USERDIR_PART_LETTER
	} type;
	union {
		GString *str;
		guint8 ndx;
	} data;
};
typedef struct userdir_part userdir_part;

static liHandlerResult userdir(liVRequest *vr, gpointer param, gpointer *context) {
	userdir_part *part;
	gchar *c;
	guint i;
	GArray *parts = param;
	gchar *username;
	guint username_len = 0;
	gboolean has_username;

	UNUSED(context);

	if (vr->request.uri.path->str[0] != '/' || vr->request.uri.path->str[1] != '~') {
		return LI_HANDLER_GO_ON;
	}

	/* only allow [a-zA-Z0-9_-] in usernames */
	for (c = vr->request.uri.path->str+2; *c != '\0'; c++) {
		if ((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == '_' || *c == '-')
			continue;
		if (*c == '/')
			break;

		return LI_HANDLER_GO_ON;
	}

	username = vr->request.uri.path->str + 2;
	username_len = c - username;
	if (username_len == 0)
		return LI_HANDLER_GO_ON;

	g_string_truncate(vr->physical.doc_root, 0);

	part = &g_array_index(parts, userdir_part, 0);

	if (part->type != USERDIR_PART_STRING || part->data.str->str[0] != '/') {
		/* pattern not starting with slash, need to lookup user's homedir */
		struct passwd pwd;
		struct passwd *result;
		gchar c_orig = *(username+username_len);

		/* do not allow root user */
		if (username_len == 4 && username[0] == 'r' && username[1] == 'o' && username[2] == 'o' && username[3] == 't') {
			if (!li_vrequest_handle_direct(vr))
				return LI_HANDLER_ERROR;

			vr->response.http_status = 403;
			return LI_HANDLER_GO_ON;
		}

		*(username+username_len) = '\0';
		while (EINTR == getpwnam_r(username, &pwd, vr->wrk->tmp_str->str, vr->wrk->tmp_str->allocated_len, &result)) {
		}
		*(username+username_len) = c_orig;

		if (!result) {
			if (!li_vrequest_handle_direct(vr))
				return LI_HANDLER_ERROR;

			vr->response.http_status = 404;
			return LI_HANDLER_GO_ON;
		}

		// user found
		g_string_append(vr->physical.doc_root, pwd.pw_dir);
		g_string_append_c(vr->physical.doc_root, G_DIR_SEPARATOR);
		has_username = TRUE;
	} else {
		has_username = FALSE;
	}

	for (i = 0; i < parts->len; i++) {
		part = &g_array_index(parts, userdir_part, i);
		switch (part->type) {
		case USERDIR_PART_STRING:
			g_string_append_len(vr->physical.doc_root, GSTR_LEN(part->data.str));
			break;
		case USERDIR_PART_USERNAME:
			g_string_append_len(vr->physical.doc_root, username, username_len);
			has_username = TRUE;
			break;
		case USERDIR_PART_LETTER:
			if (part->data.ndx <= username_len)
				g_string_append_c(vr->physical.doc_root, username[part->data.ndx-1]);
			break;
		}
	}

	if (!has_username) {
		/* pattern without username, append it. /usr/web/ => /usr/web/user/ */
		if (vr->physical.doc_root->str[vr->physical.doc_root->len-1] != G_DIR_SEPARATOR)
			g_string_append_c(vr->physical.doc_root, G_DIR_SEPARATOR);
		g_string_append_len(vr->physical.doc_root, username, username_len);
	}

	/* ensure that docroot is ending with a slash */
	if (vr->physical.doc_root->str[vr->physical.doc_root->len-1] != G_DIR_SEPARATOR)
		g_string_append_c(vr->physical.doc_root, G_DIR_SEPARATOR);

	/* build physical path: docroot + uri.path */
	g_string_truncate(vr->physical.path, 0);
	g_string_append_len(vr->physical.path, GSTR_LEN(vr->physical.doc_root));
	g_string_append_len(vr->physical.path, username + username_len, vr->request.uri.path->str - username - username_len);

	/* rewrite request path to skip username */
	g_string_truncate(vr->wrk->tmp_str, 0);
	g_string_append_len(vr->wrk->tmp_str, username + username_len, vr->request.uri.path->str - username - username_len);
	g_string_truncate(vr->request.uri.path, 0);
	if (vr->wrk->tmp_str->len)
		g_string_append_len(vr->request.uri.path, GSTR_LEN(vr->wrk->tmp_str));
	else
		g_string_append_c(vr->request.uri.path, G_DIR_SEPARATOR);

	return LI_HANDLER_GO_ON;
}

static void userdir_free(liServer *srv, gpointer param) {
	GArray *parts = param;
	guint i;

	UNUSED(srv);

	for (i = 0; i < parts->len; i++) {
		userdir_part *part = &g_array_index(parts, userdir_part, i);
		if (part->type == USERDIR_PART_STRING) {
			g_string_free(part->data.str, TRUE);
		}
	}

	g_array_free(parts, TRUE);
}

static liAction* userdir_create(liServer *srv, liPlugin* p, liValue *val, gpointer userdata) {
	GString *str;
	gchar *c, *c_last;
	GArray *parts;
	userdir_part part;

	UNUSED(p);
	UNUSED(userdata);

	if (!val || val->type != LI_VALUE_STRING) {
		ERROR(srv, "%s", "userdir expects a string as parameter");
		return NULL;
	}

	str = val->data.string;

	if (!str->len) {
		ERROR(srv, "%s", "userdir parameter must not be an empty string");
		return NULL;
	}

	parts = g_array_new(FALSE, FALSE, sizeof(userdir_part));

	// parse pattern
	for (c_last = c = str->str; c != str->str + str->len; c++) {
		if (*c == '*') {
			// username
			if (c - c_last > 0) {
				// push last string
				part.type = USERDIR_PART_STRING;
				part.data.str = g_string_new_len(c_last, c - c_last);
				g_array_append_val(parts, part);
			}

			c_last = c+1;
			part.type = USERDIR_PART_USERNAME;
			g_array_append_val(parts, part);
		} else if (*c == '$' && *(c+1) >= '1' && *(c+2) <= '9') {
			// letter
			if (c - c_last > 0) {
				// push last string
				part.type = USERDIR_PART_STRING;
				part.data.str = g_string_new_len(c_last, c - c_last);
				g_array_append_val(parts, part);
			}

			c_last = c+2;
			part.type = USERDIR_PART_LETTER;
			part.data.ndx = *(c+1) - '0';
			g_array_append_val(parts, part);
		}
	}

	if (c - c_last > 0) {
		// push last string
		part.type = USERDIR_PART_STRING;
		part.data.str = g_string_new_len(c_last, c - c_last);
		g_array_append_val(parts, part);
	}

	return li_action_new_function(userdir, NULL, userdir_free, parts);
}

static const liPluginAction actions[] = {
	{ "userdir", userdir_create, NULL },

	{ NULL, NULL, NULL }
};

static void plugin_userdir_init(liServer *srv, liPlugin *p, gpointer userdata) {
	UNUSED(srv); UNUSED(userdata);

	p->actions = actions;

}


gboolean mod_userdir_init(liModules *mods, liModule *mod) {
	UNUSED(mod);

	MODULE_VERSION_CHECK(mods);

	mod->config = li_plugin_register(mods->main, "mod_userdir", plugin_userdir_init, NULL);

	return mod->config != NULL;
}

gboolean mod_userdir_free(liModules *mods, liModule *mod) {
	if (mod->config)
		li_plugin_free(mods->main, mod->config);

	return TRUE;
}

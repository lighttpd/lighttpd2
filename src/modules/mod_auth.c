/*
 * mod_auth - require authentication from clients using username + password
 *
 * Description:
 *     mod_auth lets you require authentication from clients using a username and password.
 *     It supports basic and digest authentication methods as well as plaintext, htpass and htdigest backends.
 *
 *     Basic:
 *         The "basic" method transfers the username and the password in cleartext over the network (base64 encoded)
 *         and might result in security problems if not used in conjunction with a crypted channel between client and server.
 *         It is recommend to use https in conjunction with basic authentication.
 *
 *     Digest:
 *         The "digest" method only transfers a hashed value over the network which performs a lot of work to harden
 *         the authentication process in insecure networks (like the internet).
 *
 * Relevant RFCs: 2617
 *
 * Setups:
 *     none
 * Options:
 *     auth.debug = <true|false>;
 *         - if set, debug information is written to the log
 * Actions:
 *     auth.plain ["method": method, "realm": realm, "file": path];
 *         - requires authentication using a plaintext file containing user:password pairs seperated by newlines (\n)
 *     auth.htpasswd ["realm": realm, "file": path];
 *         - requires authentication using a plaintext file containing user:encrypted_password pairs seperated by newlines (\n)
 *         - passwords are encrypted using crypt(3), use the htpasswd binary from apache to manage the file
 *         - only supports "basic" method
 *     auth.htdigest ["method": method, "realm": realm, "file": path];
 *         - requires authentication using a plaintext file containing user:realm:hashed_password tuples seperated by newlines (\n)
 *         - passwords are saved as (modified) md5 hashes
 *
 * Example config:
 *     # /members/ is for known users only
 *     if request.path =^ "/members/" {
 *         auth ["method": "basic", "realm": "members only", "file": "/etc/lighttpd/users.txt"];
 *     }
 *
 *
 * Tip:
 *     The digest method is broken in Internet Explorer < 7. Use basic instead if this is a problem for you.
 *
 * Todo:
 *     - method: digest
 *     - auth.htdigest
 *     - auth.htpasswd
 *     - anti bruteforce protection
 *
 * Author:
 *     Copyright (c) 2009 Thomas Porzelt
 * License:
 *     MIT, see COPYING file in the lighttpd 2 tree
 */

#include <lighttpd/base.h>
#include <lighttpd/encoding.h>

LI_API gboolean mod_auth_init(liModules *mods, liModule *mod);
LI_API gboolean mod_auth_free(liModules *mods, liModule *mod);

typedef gboolean (*AuthBackend)(liVRequest *vr, const gchar *auth_info, gpointer param);

struct AuthData {
	liPlugin *p;
	GString *realm;
	AuthBackend backend;
	gpointer data;
};
typedef struct AuthData AuthData;

struct AuthFileData {
	GString *path;
	GHashTable *users;
	ev_tstamp last_check;
};
typedef struct AuthFileData AuthFileData;

static GHashTable *auth_file_load(liServer *srv, GString *path, gboolean has_realm) {
	GHashTable *users;
	gchar *contents;
	gsize len;
	gchar *c;
	gchar *user_start, *user_end;
	GString *user, *pass;
	GError *err = NULL;
	

	if (!g_file_get_contents(path->str, &contents, &len, &err)) {
		ERROR(srv, "failed to load auth file \"%s\": %s", path->str, err->message);
		g_error_free(err);
		return NULL;
	}

	users = g_hash_table_new_full(
		(GHashFunc) g_string_hash, (GEqualFunc) g_string_equal,
		(GDestroyNotify) li_string_destroy_notify, (GDestroyNotify) li_string_destroy_notify
	);

	/* parse file */
	user_start = contents;
	for (c = strchr(contents, ':'); c != NULL; c = strchr(c, ':')) {
		if (has_realm) {
			/* file is of type htdigest (user:realm:pass) */
			c = strchr(c + 1, ':');

			if (!c) {
				/* missing delimiter for realm:pass => bogus file */
				ERROR(srv, "failed to parse auth file \"%s\", doesn't look like a htdigest file", path->str);
				g_hash_table_destroy(users);
				return NULL;
			}
		}

		user_end = c - 1;
		c = strchr(c + 1, '\n');

		if (!c) {
			/* missing \n */
			ERROR(srv, "failed to parse auth file \"%s\"", path->str);
			g_hash_table_destroy(users);
			return NULL;
		}

		user = g_string_new_len(user_start, user_end - user_start + 1);
		pass = g_string_new_len(user_end + 2, c - user_end - 2);
		g_hash_table_insert(users, user, pass);

		c++;
		user_start = c;
	}

	/* c == NULL, last check if we are really at the end of the file */
	if (*user_start) {
		ERROR(srv, "failed to parse auth file \"%s\"", path->str);
		g_hash_table_destroy(users);
		return NULL;
	}

	return users;
}

static gboolean auth_backend_plain(liVRequest *vr, const gchar *auth_info, gpointer param) {
	gchar *decoded;
	gsize len;
	gchar *c;
	GString user;
	GString *pass;
	AuthData *ad = param;

	UNUSED(vr);

	/* auth_info contains username:password encoded in base64 */
	if (!(decoded = (gchar*)g_base64_decode(auth_info, &len)))
		return FALSE;

	/* bogus data? */
	if (!(c = strchr(decoded, ':'))) {
		g_free(decoded);
		return FALSE;
	}

	user.str = decoded;
	user.len = c - decoded;
	user.allocated_len = 0;

	/* unknown user? */
	if (!(pass = g_hash_table_lookup(ad->data, &user))) {
		g_free(decoded);
		return FALSE;
	}

	/* wrong password? */
	if (!g_str_equal(c+1, pass->str)) {
		g_free(decoded);
		return FALSE;
	}

	g_free(decoded);

	return TRUE;
}

static liHandlerResult auth_basic(liVRequest *vr, gpointer param, gpointer *context) {
	liHttpHeader *hdr;
	gboolean auth_ok = TRUE;
	AuthData *ad = param;
	gboolean debug = _OPTION(vr, ad->p, 0).boolean;

	UNUSED(context);

	/* check for Authorization header */
	hdr = li_http_header_lookup(vr->request.headers, CONST_STR_LEN("Authorization"));

	if (!hdr || !g_str_has_prefix(HEADER_VALUE(hdr), "Basic ")) {
		auth_ok = FALSE;

		if (debug)
			VR_DEBUG(vr, "requesting authorization from client for realm \"%s\"", ad->realm->str);
	} else if (!ad->backend(vr, HEADER_VALUE(hdr) + sizeof("Basic ") - 1, ad)) {
		auth_ok = FALSE;

		if (debug)
			VR_DEBUG(vr, "wrong authorization info from client for realm \"%s\"", ad->realm->str);
	}

	if (!auth_ok) {
		/* if the request already has a handler like mod_access, we assume everything is ok */
		if (!li_vrequest_handle_direct(vr))
			return LI_HANDLER_GO_ON;

		vr->response.http_status = 401;
		g_string_truncate(vr->wrk->tmp_str, 0);
		g_string_append_len(vr->wrk->tmp_str, CONST_STR_LEN("Basic realm=\""));
		g_string_append_len(vr->wrk->tmp_str, GSTR_LEN(ad->realm));
		g_string_append_c(vr->wrk->tmp_str, '"');
		li_http_header_overwrite(vr->response.headers, CONST_STR_LEN("WWW-Authenticate"), GSTR_LEN(vr->wrk->tmp_str));

		return LI_HANDLER_GO_ON;
	} else if (debug) {
		VR_DEBUG(vr, "client authorization successful for realm \"%s\"", ad->realm->str);
	}

	return LI_HANDLER_GO_ON;
}

static void auth_plain_free(liServer *srv, gpointer param) {
	AuthData *ad = param;

	UNUSED(srv);

	g_string_free(ad->realm, TRUE);
	g_hash_table_destroy(ad->data);
	g_slice_free(AuthData, ad);
}

static liAction* auth_plain_create(liServer *srv, liPlugin* p, liValue *val) {
	AuthData *ad;
	liValue *method, *realm, *file;
	GString str;
	GHashTable *users;


	if (!val || val->type != LI_VALUE_HASH || g_hash_table_size(val->data.hash) != 3) {
		ERROR(srv, "%s", "auth.plain expects a hashtable with 3 elements: method, realm and file");
		return NULL;
	}

	str.allocated_len = 0;
	str.str = "method";
	str.len = sizeof("method") - 1;
	method = g_hash_table_lookup(val->data.hash, &str);
	str.str = "realm";
	str.len = sizeof("realm") - 1;
	realm = g_hash_table_lookup(val->data.hash, &str);
	str.str = "file";
	str.len = sizeof("file") - 1;
	file = g_hash_table_lookup(val->data.hash, &str);

	if (!method || method->type != LI_VALUE_STRING || !realm || realm->type != LI_VALUE_STRING || !file || file->type != LI_VALUE_STRING) {
		ERROR(srv, "%s", "auth.plain expects a hashtable with 3 elements: method, realm and file");
		return NULL;
	}

	if (!g_str_equal(method->data.string->str, "basic") && !g_str_equal(method->data.string->str, "digest")) {
		ERROR(srv, "auth.plain: unknown method: %s", method->data.string->str);
		return NULL;
	}

	if (g_str_equal(method->data.string->str, "digest")) {
		ERROR(srv, "%s", "auth.plain: digest authentication not implemented yet");
		return NULL;
	}

	/* load users from file */
	users = auth_file_load(srv, file->data.string, FALSE);

	if (!users)
		return FALSE;

	ad = g_slice_new(AuthData);
	ad->p = p;
	ad->realm = li_value_extract(realm).string;
	ad->backend = auth_backend_plain;
	ad->data = users;

	if (g_str_equal(method->data.string->str, "basic"))
		return li_action_new_function(auth_basic, NULL, auth_plain_free, ad);
	else
		return NULL; /* li_action_new_function(NULL, NULL, auth_backend_plain_free, ad); */
}



static const liPluginOption options[] = {
	{ "auth.debug", LI_VALUE_BOOLEAN, NULL, NULL, NULL },

	{ NULL, 0, NULL, NULL, NULL }
};

static const liPluginAction actions[] = {
	{ "auth.plain", auth_plain_create },

	{ NULL, NULL }
};

static const liPluginSetup setups[] = {
	{ NULL, NULL }
};

static void plugin_auth_init(liServer *srv, liPlugin *p) {
	UNUSED(srv);

	p->options = options;
	p->actions = actions;
	p->setups = setups;
}


gboolean mod_auth_init(liModules *mods, liModule *mod) {
	UNUSED(mod);

	MODULE_VERSION_CHECK(mods);

	mod->config = li_plugin_register(mods->main, "mod_auth", plugin_auth_init);

	return mod->config != NULL;
}

gboolean mod_auth_free(liModules *mods, liModule *mod) {
	if (mod->config)
		li_plugin_free(mods->main, mod->config);

	return TRUE;
}

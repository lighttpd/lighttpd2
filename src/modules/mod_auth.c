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
 *           + hashes starting with "$apr1$" are NOT supported (htpasswd -m)
 *           + hashes starting with "{SHA}" ARE supported (followed by sha1_base64(password), htpasswd -s)
 *         - only supports "basic" method
 *     auth.htdigest ["method": method, "realm": realm, "file": path];
 *         - requires authentication using a plaintext file containing user:realm:hashed_password tuples seperated by newlines (\n)
 *         - passwords are saved as (modified) md5 hashes:
 *             md5hex(username + ":" + realm + ":" + password)
 *
 *     auth.deny;
 *         - handles request with "401 Unauthorized"
 *
 * Example config:
 *     # /members/ is for known users only
 *     if request.path =^ "/members/" {
 *         auth.plain ["method": "basic", "realm": "members only", "file": "/etc/lighttpd/users.txt"];
 *     }
 *     if req.env["REMOTE_USER"] !~ "^(admin1|user2|user3)$" { auth.deny; }
 *
 *
 * Tip:
 *     The digest method is broken in Internet Explorer < 7. Use basic instead if this is a problem for you. (not supported for now anyway)
 *
 * Todo:
 *     - method: digest
 *     - anti bruteforce protection
 *     - auth.deny ( using env[] "REMOTE_METHOD"/"REMOTE_REALM"/...? )
 *
 * Author:
 *     Copyright (c) 2009 Thomas Porzelt
 * License:
 *     MIT, see COPYING file in the lighttpd 2 tree
 */

#include <lighttpd/base.h>
#include <lighttpd/encoding.h>

#ifdef HAVE_CRYPT_H
# include <crypt.h>
#endif

LI_API gboolean mod_auth_init(liModules *mods, liModule *mod);
LI_API gboolean mod_auth_free(liModules *mods, liModule *mod);

typedef struct AuthBasicData AuthBasicData;

/* GStrings may be fake, only use ->str and ->len; but they are \0 terminated */
typedef gboolean (*AuthBasicBackend)(liVRequest *vr, const GString *username, const GString *password, AuthBasicData *bdata);

struct AuthBasicData {
	liPlugin *p;
	GString *realm;
	AuthBasicBackend backend;
	gpointer data;
};

typedef struct AuthFileData AuthFileData;
struct AuthFileData {
	GString *path;
	gboolean has_realm;

	GHashTable *users; /* doesn't use own strings, the strings are in contents */
	gchar *contents;

	ev_tstamp last_check; /* unused */
};

static gboolean auth_file_update(liServer *srv, AuthFileData *data) {
	GHashTable *users;
	gchar *contents;
	gchar *c;
	gchar *username, *password;
	GError *err = NULL;

	if (!g_file_get_contents(data->path->str, &contents, NULL, &err)) {
		ERROR(srv, "failed to load auth file \"%s\": %s", data->path->str, err->message);
		g_error_free(err);
		return FALSE;
	}

	users = g_hash_table_new((GHashFunc) g_str_hash, (GEqualFunc) g_str_equal);

	/* parse file */
	for ( c = contents ; *c; ) {
		gboolean found_realm, found_newline;
		username = c; password = NULL;

		found_realm = FALSE;
		found_newline = FALSE;
		for ( ; '\0' != *c ; c++ ) {
			if ('\n' == *c || '\r' == *c) {
				*c = '\0';
				found_newline = TRUE;
			} else if (':' == *c) {
				if (NULL == password) {
					password = c+1;
					*c = '\0';
				} else {
					found_realm = TRUE;
				}
			} else if (found_newline) {
				break;
			}
		}

		if (!password) {
			/* missing delimiter for user:pass => bogus file */
			ERROR(srv, "failed to parse auth file \"%s\", missing user:password delimiter", data->path->str);
			goto cleanup_fail;
		}

		/* file is of type htdigest (user:realm:pass) */
		if (data->has_realm && !found_realm) {
			/* missing delimiter for realm:pass => bogus file */
			ERROR(srv, "failed to parse auth file \"%s\", missing realm:password delimiter", data->path->str);
			goto cleanup_fail;
		}

		g_hash_table_insert(users, username, password);
	}

	/* TODO: protect update with locks? */
	if (data->contents) {
		g_free(data->contents);
		g_hash_table_destroy(data->users);
	}
	data->contents = contents;
	data->users = users;

	return TRUE;

cleanup_fail:
	g_hash_table_destroy(users);
	g_free(contents);
	return FALSE;
}

static void auth_file_free(AuthFileData* data) {
	g_string_free(data->path, TRUE);
	if (data->contents) {
		g_free(data->contents);
		g_hash_table_destroy(data->users);
	}

	g_slice_free(AuthFileData, data);
}

static AuthFileData* auth_file_new(liServer *srv, const GString *path, gboolean has_realm) {
	AuthFileData* data = g_slice_new0(AuthFileData);
	data->path = g_string_new_len(GSTR_LEN(path));
	data->has_realm = has_realm;

	if (!auth_file_update(srv, data)) {
		auth_file_free(data);
		return NULL;
	}

	return data;
}

static gboolean auth_backend_plain(liVRequest *vr, const GString *username, const GString *password, AuthBasicData *bdata) {
	const char *pass;
	AuthFileData *afd = bdata->data;

	UNUSED(vr);

	/* unknown user? */
	if (!(pass = g_hash_table_lookup(afd->users, username->str))) {
		return FALSE;
	}

	/* wrong password? */
	if (!g_str_equal(password->str, pass)) {
		return FALSE;
	}

	return TRUE;
}

static gboolean auth_backend_htpasswd(liVRequest *vr, const GString *username, const GString *password, AuthBasicData *bdata) {
	const char *pass;
	AuthFileData *afd = bdata->data;

	UNUSED(vr);

	/* unknown user? */
	if (!(pass = g_hash_table_lookup(afd->users, username->str))) {
		return FALSE;
	}

	if (g_str_has_prefix(pass, "$apr1$")) {
		/* We don't support this stupid method. Run around your house 1000 times and use sha1 next time */
		return FALSE;
	} else
	if (g_str_has_prefix(pass, "{SHA}")) {
		li_apr_sha1_base64(vr->wrk->tmp_str, password);

		if (g_str_equal(password->str, vr->wrk->tmp_str->str)) {
			return TRUE;
		}
	}
#ifdef HAVE_CRYPT_R
	else {
		struct crypt_data buffer;
		const gchar *crypted;

		memset(&buffer, 0, sizeof(buffer));
		crypted = crypt_r(password->str, pass, &buffer);

		if (g_str_equal(pass, crypted)) {
			return TRUE;
		}
	}
#endif

	return FALSE;
}

static gboolean auth_backend_htdigest(liVRequest *vr, const GString *username, const GString *password, AuthBasicData *bdata) {
	const char *pass, *realm;
	AuthFileData *afd = bdata->data;
	GChecksum *md5sum;
	gboolean res;

	UNUSED(vr);

	/* unknown user? */
	if (!(pass = g_hash_table_lookup(afd->users, username->str))) {
		return FALSE;
	}

	realm = pass;
	pass = strchr(pass, ':');

	/* no realm/wrong realm? */
	if (NULL == pass || 0 != strncmp(realm, bdata->realm->str, bdata->realm->len)) {
		return FALSE;
	}
	pass++;

	md5sum = g_checksum_new(G_CHECKSUM_MD5);
	g_checksum_update(md5sum, GUSTR_LEN(username));
	g_checksum_update(md5sum, CONST_USTR_LEN(":"));
	g_checksum_update(md5sum, GUSTR_LEN(bdata->realm));
	g_checksum_update(md5sum, CONST_USTR_LEN(":"));
	g_checksum_update(md5sum, GUSTR_LEN(password));

	res = TRUE;
	/* wrong password? */
	if (!g_str_equal(pass, g_checksum_get_string(md5sum))) {
		res = FALSE;
	}

	g_checksum_free(md5sum);

	return res;
}

static liHandlerResult auth_basic(liVRequest *vr, gpointer param, gpointer *context) {
	liHttpHeader *hdr;
	gboolean auth_ok = FALSE;
	AuthBasicData *bdata = param;
	gboolean debug = _OPTION(vr, bdata->p, 0).boolean;

	UNUSED(context);

	if (li_vrequest_is_handled(vr)) {
		/* only allow access restrictions as previous handlers */
		switch (vr->response.http_status) { /* use same list as in auth_handle_deny */
		case 401: /* Unauthorized */
		case 402: /* Payment Required */
		case 403: /* Forbidden */
		case 405: /* Method Not Allowed */
		case 407: /* Proxy Authentication Required */
		case 500: /* Internal Server Error */
			return LI_HANDLER_GO_ON;
		default:
			return LI_HANDLER_ERROR;
		}
	}

	/* check for Authorization header */
	hdr = li_http_header_lookup(vr->request.headers, CONST_STR_LEN("Authorization"));

	if (!hdr || !g_str_has_prefix(LI_HEADER_VALUE(hdr), "Basic ")) {
		if (debug) {
			VR_DEBUG(vr, "requesting authorization from client for realm \"%s\"", bdata->realm->str);
		}
	} else {
		gchar *decoded, *username = NULL, *password;
		size_t len;
		/* auth_info contains username:password encoded in base64 */
		if (NULL != (decoded = (gchar*)g_base64_decode(LI_HEADER_VALUE(hdr) + sizeof("Basic ") - 1, &len))) {
			/* bogus data? */
			if (NULL != (password = strchr(decoded, ':'))) {
				*password = '\0';
				password++;
				username = decoded;
			} else {
				g_free(decoded);
			}
		}

		if (!username) {
			if (debug) {
				VR_DEBUG(vr, "couldn't parse authorization info from client for realm \"%s\"", bdata->realm->str);
			}
		} else {
			GString user = li_const_gstring(username, password - username - 1);
			GString pass = li_const_gstring(password, len - (password - username));
			if (bdata->backend(vr, &user, &pass, bdata)) {
				auth_ok = TRUE;

				li_environment_set(&vr->env, CONST_STR_LEN("REMOTE_USER"), username, password - username - 1);
				li_environment_set(&vr->env, CONST_STR_LEN("AUTH_TYPE"), CONST_STR_LEN("Basic"));
			} else {
				if (debug) {
					VR_DEBUG(vr, "wrong authorization info from client for realm \"%s\"", bdata->realm->str);
				}
			}
			g_free(decoded);
		}
	}

	g_string_truncate(vr->wrk->tmp_str, 0);
	g_string_append_len(vr->wrk->tmp_str, CONST_STR_LEN("Basic realm=\""));
	g_string_append_len(vr->wrk->tmp_str, GSTR_LEN(bdata->realm));
	g_string_append_c(vr->wrk->tmp_str, '"');
	/* generate header always */

	if (!auth_ok) {
		li_http_header_overwrite(vr->response.headers, CONST_STR_LEN("WWW-Authenticate"), GSTR_LEN(vr->wrk->tmp_str));

		/* we already checked for handled */
		if (!li_vrequest_handle_direct(vr))
			return LI_HANDLER_ERROR;

		vr->response.http_status = 401;
		return LI_HANDLER_GO_ON;
	} else {
		/* lets hope browser just ignore the header if status is not 401
		 * but this way it is easier to use a later "auth.deny;"
		 */
		li_http_header_overwrite(vr->response.headers, CONST_STR_LEN("WWW-Authenticate"), GSTR_LEN(vr->wrk->tmp_str));
	}

	if (debug) {
		VR_DEBUG(vr, "client authorization successful for realm \"%s\"", bdata->realm->str);
	}

	return LI_HANDLER_GO_ON;
}

static void auth_basic_free(liServer *srv, gpointer param) {
	AuthBasicData *bdata = param;
	AuthFileData *afd = bdata->data;

	UNUSED(srv);

	g_string_free(bdata->realm, TRUE);
	auth_file_free(afd);

	g_slice_free(AuthBasicData, bdata);
}

static liAction* auth_generic_create(liServer *srv, liPlugin* p, liValue *val, const char *actname, AuthBasicBackend basic_action, gboolean has_realm) {
	AuthFileData *afd;
	liValue *method, *realm, *file;
	GString str;


	if (!val || val->type != LI_VALUE_HASH || g_hash_table_size(val->data.hash) != 3) {
		ERROR(srv, "%s expects a hashtable with 3 elements: method, realm and file", actname);
		return NULL;
	}

	str = li_const_gstring(CONST_STR_LEN("method"));
	method = g_hash_table_lookup(val->data.hash, &str);
	str = li_const_gstring(CONST_STR_LEN("realm"));
	realm = g_hash_table_lookup(val->data.hash, &str);
	str = li_const_gstring(CONST_STR_LEN("file"));
	file = g_hash_table_lookup(val->data.hash, &str);

	if (!method || method->type != LI_VALUE_STRING || !realm || realm->type != LI_VALUE_STRING || !file || file->type != LI_VALUE_STRING) {
		ERROR(srv, "%s expects a hashtable with 3 elements: method, realm and file", actname);
		return NULL;
	}

	if (!g_str_equal(method->data.string->str, "basic") && !g_str_equal(method->data.string->str, "digest")) {
		ERROR(srv, "%s: unknown method: %s", actname, method->data.string->str);
		return NULL;
	}

	if (g_str_equal(method->data.string->str, "digest")) {
		ERROR(srv, "%s: digest authentication not implemented yet", actname);
		return NULL;
	}

	/* load users from file */
	afd = auth_file_new(srv, file->data.string, has_realm);

	if (!afd)
		return FALSE;

	if (g_str_equal(method->data.string->str, "basic")) {
		AuthBasicData *bdata;

		bdata = g_slice_new(AuthBasicData);
		bdata->p = p;
		bdata->realm = li_value_extract(realm).string;
		bdata->backend = basic_action;
		bdata->data = afd;

		return li_action_new_function(auth_basic, NULL, auth_basic_free, bdata);
	} else {
		auth_file_free(afd);
		return NULL; /* li_action_new_function(NULL, NULL, auth_backend_plain_free, ad); */
	}
}


static liAction* auth_plain_create(liServer *srv, liPlugin* p, liValue *val) {
	return auth_generic_create(srv, p, val, "auth.plain", auth_backend_plain, FALSE);
}

static liAction* auth_htpasswd_create(liServer *srv, liPlugin* p, liValue *val) {
	return auth_generic_create(srv, p, val, "auth.htpasswd", auth_backend_htpasswd, FALSE);
}

static liAction* auth_htdigest_create(liServer *srv, liPlugin* p, liValue *val) {
	return auth_generic_create(srv, p, val, "auth.htdigest", auth_backend_htdigest, TRUE);
}

static liHandlerResult auth_handle_deny(liVRequest *vr, gpointer param, gpointer *context) {
	UNUSED(param);
	UNUSED(context);

	if (!li_vrequest_handle_direct(vr)) {
		/* only allow access restrictions as previous handlers */
		switch (vr->response.http_status) { /* use same list as in auth_basic */
		case 401: /* Unauthorized */
		case 402: /* Payment Required */
		case 403: /* Forbidden */
		case 405: /* Method Not Allowed */
		case 407: /* Proxy Authentication Required */
		case 500: /* Internal Server Error */
			return LI_HANDLER_GO_ON;
		default:
			return LI_HANDLER_ERROR;
		}
	}

	vr->response.http_status = 401;

	return LI_HANDLER_GO_ON;
}

static liAction* auth_deny(liServer *srv, liPlugin* p, liValue *val) {
	UNUSED(srv);
	UNUSED(p);

	if (val) {
		ERROR(srv, "%s", "'auth.deny' action doesn't have parameters");
		return NULL;
	}

	return li_action_new_function(auth_handle_deny, NULL, NULL, NULL);
}

static const liPluginOption options[] = {
	{ "auth.debug", LI_VALUE_BOOLEAN, NULL, NULL, NULL },

	{ NULL, 0, NULL, NULL, NULL }
};

static const liPluginAction actions[] = {
	{ "auth.plain", auth_plain_create },
	{ "auth.htpasswd", auth_htpasswd_create },
	{ "auth.htdigest", auth_htdigest_create },

	{ "auth.deny", auth_deny },

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

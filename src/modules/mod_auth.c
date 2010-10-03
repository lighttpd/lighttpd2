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
 *     auth.plain ["method": method, "realm": realm, "file": path, "ttl": 10];
 *         - requires authentication using a plaintext file containing user:password pairs seperated by newlines (\n)
 *     auth.htpasswd ["method": method, "realm": realm, "file": path, "ttl": 10];
 *         - requires authentication using a htpasswd file containing user:encrypted_password pairs seperated by newlines (\n)
 *         - passwords are encrypted using crypt(3), use the htpasswd binary from apache to manage the file
 *           + hashes starting with "$apr1$" are NOT supported (htpasswd -m)
 *           + hashes starting with "{SHA}" ARE supported (followed by sha1_base64(password), htpasswd -s)
 *         - only supports "basic" method
 *     auth.htdigest ["method": method, "realm": realm, "file": path, "ttl": 10];
 *         - requires authentication using a htdigest file containing user:realm:hashed_password tuples seperated by newlines (\n)
 *         - passwords are saved as (modified) md5 hashes:
 *             md5hex(username + ":" + realm + ":" + password)
 *
 *     ttl specifies how often lighty checks the files for modifications (in seconds), 0 means it will never check after the first load.
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
 *
 * Author:
 *     Copyright (c) 2009 Thomas Porzelt
 * License:
 *     MIT, see COPYING file in the lighttpd 2 tree
 */

#include <lighttpd/base.h>
#include <lighttpd/encoding.h>

#include <lighttpd/plugin_core.h>

#ifdef HAVE_CRYPT_H
# include <crypt.h>
#endif

LI_API gboolean mod_auth_init(liModules *mods, liModule *mod);
LI_API gboolean mod_auth_free(liModules *mods, liModule *mod);

typedef struct AuthBasicData AuthBasicData;

/* GStrings may be fake, only use ->str and ->len; but they are \0 terminated */
typedef gboolean (*AuthBasicBackend)(liVRequest *vr, const GString *username, const GString *password, AuthBasicData *bdata, gboolean debug);

struct AuthBasicData {
	liPlugin *p;
	GString *realm;
	AuthBasicBackend backend;
	gpointer data;
};

typedef struct AuthFileData AuthFileData;
struct AuthFileData {
	int refcount;

	GHashTable *users; /* doesn't use own strings, the strings are in contents */
	gchar *contents;
};

typedef struct AuthFile AuthFile;
struct AuthFile {
	GString *path;
	gboolean has_realm;

	GMutex *lock;

	AuthFileData *data;
	ev_tstamp last_stat;

	gint ttl;
	ev_tstamp next_check; /* unused */
};

static AuthFileData* auth_file_load(liServer *srv, AuthFile *f) {
	GHashTable *users;
	gchar *contents;
	gchar *c;
	gchar *username, *password;
	GError *err = NULL;
	AuthFileData *data = NULL;

	if (!g_file_get_contents(f->path->str, &contents, NULL, &err)) {
		ERROR(srv, "failed to load auth file \"%s\": %s", f->path->str, err->message);
		g_error_free(err);
		return NULL;
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
			ERROR(srv, "failed to parse auth file \"%s\", missing user:password delimiter", f->path->str);
			goto cleanup_fail;
		}

		/* file is of type htdigest (user:realm:pass) */
		if (f->has_realm && !found_realm) {
			/* missing delimiter for realm:pass => bogus file */
			ERROR(srv, "failed to parse auth file \"%s\", missing realm:password delimiter", f->path->str);
			goto cleanup_fail;
		}

		g_hash_table_insert(users, username, password);
	}

	data = g_slice_new(AuthFileData);
	data->refcount = 1;
	data->contents = contents;
	data->users = users;

	return data;

cleanup_fail:
	g_hash_table_destroy(users);
	g_free(contents);
	return NULL;
}

static void auth_file_data_release(AuthFileData *data) {
	if (!data) return;
	assert(g_atomic_int_get(&data->refcount) > 0);
	if (!g_atomic_int_dec_and_test(&data->refcount)) return;

	g_hash_table_destroy(data->users);
	g_free(data->contents);
	g_slice_free(AuthFileData, data);
}

static AuthFileData* auth_file_get_data(liWorker *wrk, AuthFile *f) {
	ev_tstamp now = ev_now(wrk->loop);
	AuthFileData *data = NULL;

	g_mutex_lock(f->lock);

	if (f->ttl != 0 && now >= f->next_check) {
		struct stat st;
		f->next_check = now + f->ttl;

		if (-1 != stat(f->path->str, &st) && st.st_mtime >= f->last_stat - 1) {
			g_mutex_unlock(f->lock);

			/* update without lock held */
			data = auth_file_load(wrk->srv, f);

			g_mutex_lock(f->lock);

			if (NULL != data) {
				auth_file_data_release(f->data);
				f->data = data;
			}
		}

		f->last_stat = now;
	}

	data = f->data;
	if (NULL != data) g_atomic_int_inc(&data->refcount);

	g_mutex_unlock(f->lock);

	return data;
}

static void auth_file_free(AuthFile* f) {
	if (NULL == f) return;

	g_string_free(f->path, TRUE);
	auth_file_data_release(f->data);
	g_mutex_free(f->lock);

	g_slice_free(AuthFile, f);
}

static AuthFile* auth_file_new(liWorker *wrk, const GString *path, gboolean has_realm, gint ttl) {
	AuthFile* f = g_slice_new0(AuthFile);
	f->path = g_string_new_len(GSTR_LEN(path));
	f->has_realm = has_realm;
	f->ttl = ttl;
	f->next_check = ev_now(wrk->loop) + ttl;
	f->lock = g_mutex_new();

	if (NULL == (f->data = auth_file_load(wrk->srv, f))) {
		auth_file_free(f);
		return NULL;
	}

	return f;
}

static gboolean auth_backend_plain(liVRequest *vr, const GString *username, const GString *password, AuthBasicData *bdata, gboolean debug) {
	const char *pass;
	AuthFileData *afd = auth_file_get_data(vr->wrk, bdata->data);
	gboolean res = FALSE;

	if (NULL == afd) return FALSE;

	/* unknown user? */
	if (!(pass = g_hash_table_lookup(afd->users, username->str))) {
		if (debug) {
			VR_DEBUG(vr, "User \"%s\" not found", username->str);
		}
		goto out;
	}

	/* wrong password? */
	if (0 != g_strcmp0(password->str, pass)) {
		if (debug) {
			VR_DEBUG(vr, "Password \"%s\" doesn't match \"%s\" for user \"%s\"", password->str, pass, username->str);
		}

		goto out;
	}

	res = TRUE;

out:
	auth_file_data_release(afd);

	return res;
}

static gboolean auth_backend_htpasswd(liVRequest *vr, const GString *username, const GString *password, AuthBasicData *bdata, gboolean debug) {
	const char *pass;
	AuthFileData *afd = auth_file_get_data(vr->wrk, bdata->data);
	gboolean res = FALSE;

	if (NULL == afd) return FALSE;

	/* unknown user? */
	if (!(pass = g_hash_table_lookup(afd->users, username->str))) {
		if (debug) {
			VR_DEBUG(vr, "User \"%s\" not found", username->str);
		}
		goto out;
	}

	if (g_str_has_prefix(pass, "$apr1$")) {
		const GString salt = { (gchar*) pass, strlen(pass), 0 };
		li_apr_md5_crypt(vr->wrk->tmp_str, password, &salt);

		if (0 != g_strcmp0(pass, vr->wrk->tmp_str->str)) {
			if (debug) {
				VR_DEBUG(vr, "Password apr-md5 crypt \"%s\" doesn't match \"%s\" for user \"%s\"", vr->wrk->tmp_str->str, pass, username->str);
			}
			goto out;
		}
	} else
	if (g_str_has_prefix(pass, "{SHA}")) {
		li_apr_sha1_base64(vr->wrk->tmp_str, password);

		if (0 != g_strcmp0(pass, vr->wrk->tmp_str->str)) {
			if (debug) {
				VR_DEBUG(vr, "Password apr-sha1 crypt \"%s\" doesn't match \"%s\" for user \"%s\"", vr->wrk->tmp_str->str, pass, username->str);
			}
			goto out;
		}
	}
#ifdef HAVE_CRYPT_R
	else {
		struct crypt_data buffer;
		const gchar *crypted;

		memset(&buffer, 0, sizeof(buffer));
		crypted = crypt_r(password->str, pass, &buffer);

		if (0 != g_strcmp0(pass, crypted)) {
			if (debug) {
				VR_DEBUG(vr, "Password crypt \"%s\" doesn't match \"%s\" for user \"%s\"", crypted, pass, username->str);
			}
			goto out;
		}
	}
#endif

	res = TRUE;

out:
	auth_file_data_release(afd);

	return res;
}

static gboolean auth_backend_htdigest(liVRequest *vr, const GString *username, const GString *password, AuthBasicData *bdata, gboolean debug) {
	const char *pass, *realm;
	AuthFileData *afd = auth_file_get_data(vr->wrk, bdata->data);
	GChecksum *md5sum;
	gboolean res = FALSE;

	if (NULL == afd) return FALSE;

	/* unknown user? */
	if (!(pass = g_hash_table_lookup(afd->users, username->str))) {
		if (debug) {
			VR_DEBUG(vr, "User \"%s\" not found", username->str);
		}
		goto out;
	}

	realm = pass;
	pass = strchr(pass, ':');

	/* no realm/wrong realm? */
	if (NULL == pass || 0 != strncmp(realm, bdata->realm->str, bdata->realm->len)) {
		if (debug) {
			VR_DEBUG(vr, "Realm for user \"%s\" doesn't match", username->str);
		}
		goto out;
	}
	pass++;

	md5sum = g_checksum_new(G_CHECKSUM_MD5);
	g_checksum_update(md5sum, GUSTR_LEN(username));
	g_checksum_update(md5sum, CONST_USTR_LEN(":"));
	g_checksum_update(md5sum, GUSTR_LEN(bdata->realm));
	g_checksum_update(md5sum, CONST_USTR_LEN(":"));
	g_checksum_update(md5sum, GUSTR_LEN(password));

	/* wrong password? */
	if (g_str_equal(pass, g_checksum_get_string(md5sum))) {
		res = TRUE;
	} else {
		if (debug) {
			VR_DEBUG(vr, "Password digest \"%s\" doesn't match \"%s\" for user \"%s\"", g_checksum_get_string(md5sum), pass, username->str);
		}
	}

	g_checksum_free(md5sum);

out:
	auth_file_data_release(afd);

	return res;
}

static liHandlerResult auth_basic(liVRequest *vr, gpointer param, gpointer *context) {
	liHttpHeader *hdr;
	gboolean auth_ok = FALSE;
	AuthBasicData *bdata = param;
	gboolean debug = _OPTION(vr, bdata->p, 0).boolean;

	UNUSED(context);

	if (li_vrequest_is_handled(vr)) {
		if (debug || CORE_OPTION(LI_CORE_OPTION_DEBUG_REQUEST_HANDLING).boolean) {
			VR_DEBUG(vr, "skipping auth.basic as request is already handled with current status %i", vr->response.http_status);
		}
		return LI_HANDLER_GO_ON;
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
			if (bdata->backend(vr, &user, &pass, bdata, debug)) {
				auth_ok = TRUE;

				li_environment_set(&vr->env, CONST_STR_LEN("REMOTE_USER"), username, password - username - 1);
				li_environment_set(&vr->env, CONST_STR_LEN("AUTH_TYPE"), CONST_STR_LEN("Basic"));
			} else {
				if (debug) {
					VR_DEBUG(vr, "wrong authorization info from client on realm \"%s\" (user: \"%s\")", bdata->realm->str, username);
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
	AuthFile *afd = bdata->data;

	UNUSED(srv);

	g_string_free(bdata->realm, TRUE);
	auth_file_free(afd);

	g_slice_free(AuthBasicData, bdata);
}

/* auth option names */
static const GString
	aon_method = { CONST_STR_LEN("method"), 0 },
	aon_realm = { CONST_STR_LEN("realm"), 0 },
	aon_file = { CONST_STR_LEN("file"), 0 },
	aon_ttl = { CONST_STR_LEN("ttl"), 0 }
;

static liAction* auth_generic_create(liServer *srv, liWorker *wrk, liPlugin* p, liValue *val, const char *actname, AuthBasicBackend basic_action, gboolean has_realm) {
	AuthFile *afd;
	liValue *method = NULL, *realm = NULL, *file = NULL;
	gint ttl = 10;

	GHashTableIter it;
	gpointer pkey, pvalue;

	if (!val || val->type != LI_VALUE_HASH) {
		ERROR(srv, "%s expects a hashtable with at least 3 elements: method, realm and file", actname);
		return NULL;
	}

	g_hash_table_iter_init(&it, val->data.hash);
	while (g_hash_table_iter_next(&it, &pkey, &pvalue)) {
		GString *key = pkey;
		liValue *value = pvalue;

		if (g_string_equal(key, &aon_method)) {
			if (value->type != LI_VALUE_STRING) {
				ERROR(srv, "auth option '%s' expects string as parameter", aon_method.str);
				return NULL;
			}
			method = value;
		} else if (g_string_equal(key, &aon_realm)) {
			if (value->type != LI_VALUE_STRING) {
				ERROR(srv, "auth option '%s' expects string as parameter", aon_realm.str);
				return NULL;
			}
			realm = value;
		} else if (g_string_equal(key, &aon_file)) {
			if (value->type != LI_VALUE_STRING) {
				ERROR(srv, "auth option '%s' expects string as parameter", aon_file.str);
				return NULL;
			}
			file = value;
		} else if (g_string_equal(key, &aon_ttl)) {
			if (value->type != LI_VALUE_NUMBER || value->data.number < 0) {
				ERROR(srv, "auth option '%s' expects non-negative number as parameter", aon_ttl.str);
				return NULL;
			}
			ttl = value->data.number;
		}
	}

	if (NULL == method || NULL == realm || NULL == file) {
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
	afd = auth_file_new(wrk, file->data.string, has_realm, ttl);

	if (!afd)
		return FALSE;

	if (g_str_equal(method->data.string->str, "basic")) {
		AuthBasicData *bdata;

		bdata = g_slice_new(AuthBasicData);
		bdata->p = p;
		bdata->realm = li_value_extract_string(realm);
		bdata->backend = basic_action;
		bdata->data = afd;

		return li_action_new_function(auth_basic, NULL, auth_basic_free, bdata);
	} else {
		auth_file_free(afd);
		return NULL; /* li_action_new_function(NULL, NULL, auth_backend_plain_free, ad); */
	}
}


static liAction* auth_plain_create(liServer *srv, liWorker *wrk, liPlugin* p, liValue *val, gpointer userdata) {
	UNUSED(srv); UNUSED(userdata);
	return auth_generic_create(srv, wrk, p, val, "auth.plain", auth_backend_plain, FALSE);
}

static liAction* auth_htpasswd_create(liServer *srv, liWorker *wrk, liPlugin* p, liValue *val, gpointer userdata) {
	UNUSED(srv); UNUSED(userdata);
	return auth_generic_create(srv, wrk, p, val, "auth.htpasswd", auth_backend_htpasswd, FALSE);
}

static liAction* auth_htdigest_create(liServer *srv, liWorker *wrk, liPlugin* p, liValue *val, gpointer userdata) {
	UNUSED(srv); UNUSED(userdata);
	return auth_generic_create(srv, wrk, p, val, "auth.htdigest", auth_backend_htdigest, TRUE);
}

static liHandlerResult auth_handle_deny(liVRequest *vr, gpointer param, gpointer *context) {
	AuthBasicData *bdata = param;
	UNUSED(context);

	if (!li_vrequest_handle_direct(vr)) {
		if (_OPTION(vr, bdata->p, 0).boolean || CORE_OPTION(LI_CORE_OPTION_DEBUG_REQUEST_HANDLING).boolean) {
			VR_DEBUG(vr, "skipping auth.deny as request is already handled with current status %i", vr->response.http_status);
		}
		return LI_HANDLER_GO_ON;
	}

	vr->response.http_status = 401;

	return LI_HANDLER_GO_ON;
}

static liAction* auth_deny(liServer *srv, liWorker *wrk, liPlugin* p, liValue *val, gpointer userdata) {
	UNUSED(srv);
	UNUSED(wrk);
	UNUSED(p);
	UNUSED(userdata);

	if (val) {
		ERROR(srv, "%s", "'auth.deny' action doesn't have parameters");
		return NULL;
	}

	return li_action_new_function(auth_handle_deny, NULL, NULL, NULL);
}

static const liPluginOption options[] = {
	{ "auth.debug", LI_VALUE_BOOLEAN, 0, NULL },

	{ NULL, 0, 0, NULL }
};

static const liPluginAction actions[] = {
	{ "auth.plain", auth_plain_create, NULL },
	{ "auth.htpasswd", auth_htpasswd_create, NULL },
	{ "auth.htdigest", auth_htdigest_create, NULL },

	{ "auth.deny", auth_deny, NULL },

	{ NULL, NULL, NULL }
};

static const liPluginSetup setups[] = {
	{ NULL, NULL, NULL }
};

static void plugin_auth_init(liServer *srv, liPlugin *p, gpointer userdata) {
	UNUSED(srv);
	UNUSED(userdata);

	p->options = options;
	p->actions = actions;
	p->setups = setups;
}


gboolean mod_auth_init(liModules *mods, liModule *mod) {
	UNUSED(mod);

	MODULE_VERSION_CHECK(mods);

	mod->config = li_plugin_register(mods->main, "mod_auth", plugin_auth_init, NULL);

	return mod->config != NULL;
}

gboolean mod_auth_free(liModules *mods, liModule *mod) {
	if (mod->config)
		li_plugin_free(mods->main, mod->config);

	return TRUE;
}

/*
 * mod_auth - require authentication from clients using username + password
 *
 * Relevant RFCs: 2617
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
	li_tstamp last_stat;

	gint ttl;
	li_tstamp next_check; /* unused */
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
	LI_FORCE_ASSERT(g_atomic_int_get(&data->refcount) > 0);
	if (!g_atomic_int_dec_and_test(&data->refcount)) return;

	g_hash_table_destroy(data->users);
	g_free(data->contents);
	g_slice_free(AuthFileData, data);
}

static AuthFileData* auth_file_get_data(liWorker *wrk, AuthFile *f) {
	li_tstamp now = li_cur_ts(wrk);
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
	f->next_check = li_cur_ts(wrk) + ttl;
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

	/* unknown user or empty crypt? */
	if (NULL == (pass = g_hash_table_lookup(afd->users, username->str)) || '\0' == pass[0]) {
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
	} else if (g_str_has_prefix(pass, "{SHA}")) {
		li_apr_sha1_base64(vr->wrk->tmp_str, password);

		if (0 != g_strcmp0(pass, vr->wrk->tmp_str->str)) {
			if (debug) {
				VR_DEBUG(vr, "Password apr-sha1 crypt \"%s\" doesn't match \"%s\" for user \"%s\"", vr->wrk->tmp_str->str, pass, username->str);
			}
			goto out;
		}
	} else {
		const GString salt = { (gchar*) pass, strlen(pass), 0 };

		if (!li_safe_crypt(vr->wrk->tmp_str, password, &salt)) {
			if (debug) {
				VR_DEBUG(vr, "Invalid password salt/hash \"%s\" for user \"%s\"", pass, username->str);
			}
			goto out;
		}

		if (0 != g_strcmp0(pass, vr->wrk->tmp_str->str)) {
			if (debug) {
				VR_DEBUG(vr, "Password crypt \"%s\" doesn't match \"%s\" for user \"%s\"", vr->wrk->tmp_str->str, pass, username->str);
			}
			goto out;
		}
	}

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
	li_g_string_append_len(vr->wrk->tmp_str, CONST_STR_LEN("Basic realm=\""));
	li_g_string_append_len(vr->wrk->tmp_str, GSTR_LEN(bdata->realm));
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
	GString *method = NULL, *file = NULL;
	liValue *realm = NULL;
	gboolean have_ttl_parameter = FALSE;
	gint ttl = 10;

	val = li_value_get_single_argument(val);

	if (NULL == (val = li_value_to_key_value_list(val))) {
		ERROR(srv, "%s expects a hashtable/key-value list with at least 3 elements: method, realm and file", actname);
		return NULL;
	}

	LI_VALUE_FOREACH(entry, val)
		liValue *entryKey = li_value_list_at(entry, 0);
		liValue *entryValue = li_value_list_at(entry, 1);
		GString *entryKeyStr;

		if (LI_VALUE_NONE == li_value_type(entryKey)) {
			ERROR(srv, "%s doesn't take default keys", actname);
			return NULL;
		}
		entryKeyStr = entryKey->data.string; /* keys are either NONE or STRING */

		if (g_string_equal(entryKeyStr, &aon_method)) {
			if (LI_VALUE_STRING != li_value_type(entryValue)) {
				ERROR(srv, "auth option '%s' expects string as parameter", entryKeyStr->str);
				return NULL;
			}
			if (NULL != method) {
				ERROR(srv, "duplicate auth option '%s'", entryKeyStr->str);
				return NULL;
			}
			method = entryValue->data.string;
		} else if (g_string_equal(entryKeyStr, &aon_realm)) {
			if (LI_VALUE_STRING != li_value_type(entryValue)) {
				ERROR(srv, "auth option '%s' expects string as parameter", entryKeyStr->str);
				return NULL;
			}
			if (NULL != realm) {
				ERROR(srv, "duplicate auth option '%s'", entryKeyStr->str);
				return NULL;
			}
			realm = entryValue;
		} else if (g_string_equal(entryKeyStr, &aon_file)) {
			if (LI_VALUE_STRING != li_value_type(entryValue)) {
				ERROR(srv, "auth option '%s' expects string as parameter", entryKeyStr->str);
				return NULL;
			}
			if (NULL != file) {
				ERROR(srv, "duplicate auth option '%s'", entryKeyStr->str);
				return NULL;
			}
			file = entryValue->data.string;
		} else if (g_string_equal(entryKeyStr, &aon_ttl)) {
			if (LI_VALUE_NUMBER != li_value_type(entryValue) || entryValue->data.number < 0) {
				ERROR(srv, "auth option '%s' expects non-negative number as parameter", entryKeyStr->str);
				return NULL;
			}
			if (have_ttl_parameter) {
				ERROR(srv, "duplicate auth option '%s'", entryKeyStr->str);
				return NULL;
			}
			have_ttl_parameter = TRUE;
			ttl = entryValue->data.number;
		} else {
			ERROR(srv, "unknown auth option '%s'", entryKeyStr->str);
			return NULL;
		}
	LI_VALUE_END_FOREACH()

	if (NULL == method || NULL == realm || NULL == file) {
		ERROR(srv, "%s expects a hashtable/key-value list with 3 elements: method, realm and file", actname);
		return NULL;
	}

	if (!g_str_equal(method->str, "basic") && !g_str_equal(method->str, "digest")) {
		ERROR(srv, "%s: unknown method: %s", actname, method->str);
		return NULL;
	}

	if (g_str_equal(method->str, "digest")) {
		ERROR(srv, "%s: digest authentication not implemented yet", actname);
		return NULL;
	}

	/* load users from file */
	afd = auth_file_new(wrk, file, has_realm, ttl);

	if (!afd)
		return FALSE;

	if (g_str_equal(method->str, "basic")) {
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
	liPlugin *p = param;
	UNUSED(context);

	if (!li_vrequest_handle_direct(vr)) {
		if (_OPTION(vr, p, 0).boolean || CORE_OPTION(LI_CORE_OPTION_DEBUG_REQUEST_HANDLING).boolean) {
			VR_DEBUG(vr, "skipping auth.deny as request is already handled with current status %i", vr->response.http_status);
		}
		return LI_HANDLER_GO_ON;
	}

	vr->response.http_status = 403;

	return LI_HANDLER_GO_ON;
}

static liAction* auth_deny(liServer *srv, liWorker *wrk, liPlugin* p, liValue *val, gpointer userdata) {
	UNUSED(srv);
	UNUSED(wrk);
	UNUSED(userdata);

	if (!li_value_is_nothing(val)) {
		ERROR(srv, "%s", "'auth.deny' action doesn't have parameters");
		return NULL;
	}

	return li_action_new_function(auth_handle_deny, NULL, NULL, p);
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

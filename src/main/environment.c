
#include <lighttpd/base.h>
#include <lighttpd/plugin_core.h>
#include <lighttpd/utils.h>

static void _hash_free_gstring(gpointer data) {
	g_string_free((GString*) data, TRUE);
}

void li_environment_init(liEnvironment *env) {
	env->table = g_hash_table_new_full(
		(GHashFunc) g_string_hash, (GEqualFunc) g_string_equal,
		_hash_free_gstring, _hash_free_gstring);
}

void li_environment_reset(liEnvironment *env) {
	g_hash_table_remove_all(env->table);
}

void li_environment_clear(liEnvironment *env) {
	g_hash_table_destroy(env->table);
	env->table = NULL;
}

void li_environment_set(liEnvironment *env, const gchar *key, size_t keylen, const gchar *val, size_t valuelen) {
	GString *skey = g_string_new_len(key, keylen);
	GString *sval = g_string_new_len(val, valuelen);
	g_hash_table_insert(env->table, skey, sval);
}

void li_environment_insert(liEnvironment *env, const gchar *key, size_t keylen, const gchar *val, size_t valuelen) {
	GString *sval = li_environment_get(env, key, keylen), *skey;
	if (!sval) {
		skey = g_string_new_len(key, keylen);
		sval = g_string_new_len(val, valuelen);
		g_hash_table_insert(env->table, skey, sval);
	}
}

void li_environment_remove(liEnvironment *env, const gchar *key, size_t keylen) {
	const GString skey = li_const_gstring(key, keylen); /* fake a constant GString */
	g_hash_table_remove(env->table, &skey);
}

GString* li_environment_get(liEnvironment *env, const gchar *key, size_t keylen) {
	const GString skey = li_const_gstring(key, keylen); /* fake a constant GString */
	return (GString*) g_hash_table_lookup(env->table, &skey);
}

liEnvironmentDup* li_environment_make_dup(liEnvironment *env) {
	GHashTableIter i;
	GHashTable *tdst;
	gpointer key, val;
	liEnvironmentDup *envdup = g_slice_new0(liEnvironmentDup);
	envdup->table = tdst = g_hash_table_new((GHashFunc) g_string_hash, (GEqualFunc) g_string_equal);

	g_hash_table_iter_init(&i, env->table);
	while (g_hash_table_iter_next(&i, &key, &val)) {
		g_hash_table_insert(tdst, key, val);
	}
	return envdup;
}

void li_environment_dup_free(liEnvironmentDup *envdup) {
	g_hash_table_destroy(envdup->table);
	g_slice_free(liEnvironmentDup, envdup);
}

GString* li_environment_dup_pop(liEnvironmentDup *envdup, const gchar *key, size_t keylen) {
	const GString skey = li_const_gstring(key, keylen); /* fake a constant GString */
	GString *sval = (GString*) g_hash_table_lookup(envdup->table, &skey);
	if (sval) g_hash_table_remove(envdup->table, &skey);
	return sval;
}

static void add_env_var(liEnvironmentDup *envdup, liAddEnvironmentCB callback, gpointer param, const gchar *key, size_t keylen, const gchar *val, size_t valuelen) {
	GString *sval;

	if (NULL != (sval = li_environment_dup_pop(envdup, key, keylen))) {
		callback(param, key, keylen, GSTR_LEN(sval));
	} else {
		callback(param, key, keylen, val, valuelen);
	}
}

static void cgi_fix_header_name(GString *str) {
	guint i, len = str->len;
	gchar *s = str->str;
	for (i = 0; i < len; i++) {
		if (g_ascii_isalpha(s[i])) {
			s[i] = g_ascii_toupper(s[i]);
		} else if (!g_ascii_isdigit(s[i])) {
			s[i] = '_';
		}
	}
}

void li_environment_dup2cgi(liVRequest *vr, liEnvironmentDup *envdup, liAddEnvironmentCB callback, gpointer param) {
	liConInfo *coninfo = vr->coninfo;
	GString *tmp = vr->wrk->tmp_str;

	/* SCGI needs this as first variable */
	if (vr->request.content_length >= 0) {
		g_string_printf(tmp, "%" LI_GOFFSET_MODIFIER "i", vr->request.content_length);
		add_env_var(envdup, callback, param, CONST_STR_LEN("CONTENT_LENGTH"), GSTR_LEN(tmp));
	}

	add_env_var(envdup, callback, param, CONST_STR_LEN("SERVER_SOFTWARE"), GSTR_LEN(CORE_OPTIONPTR(LI_CORE_OPTION_SERVER_TAG).string));
	add_env_var(envdup, callback, param, CONST_STR_LEN("SERVER_NAME"), GSTR_LEN(vr->request.uri.host));
	add_env_var(envdup, callback, param, CONST_STR_LEN("GATEWAY_INTERFACE"), CONST_STR_LEN("CGI/1.1"));
	{
		guint port = 0;
		switch (coninfo->local_addr.addr_up.plain->sa_family) {
		case AF_INET: port = coninfo->local_addr.addr_up.ipv4->sin_port; break;
#ifdef HAVE_IPV6
		case AF_INET6: port = coninfo->local_addr.addr_up.ipv6->sin6_port; break;
#endif
		}
		if (port) {
			g_string_printf(tmp, "%u", htons(port));
			add_env_var(envdup, callback, param, CONST_STR_LEN("SERVER_PORT"), GSTR_LEN(tmp));
		}
	}
	add_env_var(envdup, callback, param, CONST_STR_LEN("SERVER_ADDR"), GSTR_LEN(coninfo->local_addr_str));

	{
		guint port = 0;
		switch (coninfo->remote_addr.addr_up.plain->sa_family) {
		case AF_INET: port = coninfo->remote_addr.addr_up.ipv4->sin_port; break;
#ifdef HAVE_IPV6
		case AF_INET6: port = coninfo->remote_addr.addr_up.ipv6->sin6_port; break;
#endif
		}
		if (port) {
			g_string_printf(tmp, "%u", htons(port));
			add_env_var(envdup, callback, param, CONST_STR_LEN("REMOTE_PORT"), GSTR_LEN(tmp));
		}
	}
	add_env_var(envdup, callback, param, CONST_STR_LEN("REMOTE_ADDR"), GSTR_LEN(coninfo->remote_addr_str));

	add_env_var(envdup, callback, param, CONST_STR_LEN("SCRIPT_NAME"), GSTR_LEN(vr->request.uri.path));

	add_env_var(envdup, callback, param, CONST_STR_LEN("PATH_INFO"), GSTR_LEN(vr->physical.pathinfo));
	if (vr->physical.pathinfo->len) {
		g_string_truncate(tmp, 0);
		g_string_append_len(tmp, GSTR_LEN(vr->physical.doc_root)); /* TODO: perhaps an option for alternative doc-root? */
		g_string_append_len(tmp, GSTR_LEN(vr->physical.pathinfo));
		add_env_var(envdup, callback, param, CONST_STR_LEN("PATH_TRANSLATED"), GSTR_LEN(tmp));
	}

	add_env_var(envdup, callback, param, CONST_STR_LEN("SCRIPT_FILENAME"), GSTR_LEN(vr->physical.path));
	add_env_var(envdup, callback, param, CONST_STR_LEN("DOCUMENT_ROOT"), GSTR_LEN(vr->physical.doc_root));

	add_env_var(envdup, callback, param, CONST_STR_LEN("REQUEST_URI"), GSTR_LEN(vr->request.uri.raw_orig_path));
	if (!g_string_equal(vr->request.uri.raw_orig_path, vr->request.uri.raw_path)) {
		add_env_var(envdup, callback, param, CONST_STR_LEN("REDIRECT_URI"), GSTR_LEN(vr->request.uri.raw_path));
	}
	add_env_var(envdup, callback, param, CONST_STR_LEN("QUERY_STRING"), GSTR_LEN(vr->request.uri.query));

	add_env_var(envdup, callback, param, CONST_STR_LEN("REQUEST_METHOD"), GSTR_LEN(vr->request.http_method_str));
	add_env_var(envdup, callback, param, CONST_STR_LEN("REDIRECT_STATUS"), CONST_STR_LEN("200")); /* if php is compiled with --force-redirect */
	switch (vr->request.http_version) {
	case LI_HTTP_VERSION_1_1:
		add_env_var(envdup, callback, param, CONST_STR_LEN("SERVER_PROTOCOL"), CONST_STR_LEN("HTTP/1.1"));
		break;
	case LI_HTTP_VERSION_1_0:
	default:
		add_env_var(envdup, callback, param, CONST_STR_LEN("SERVER_PROTOCOL"), CONST_STR_LEN("HTTP/1.0"));
		break;
	}

	if (coninfo->is_ssl) {
		add_env_var(envdup, callback, param, CONST_STR_LEN("HTTPS"), CONST_STR_LEN("on"));
		add_env_var(envdup, callback, param, CONST_STR_LEN("REQUEST_SCHEME"), CONST_STR_LEN("https"));
	} else {
		add_env_var(envdup, callback, param, CONST_STR_LEN("REQUEST_SCHEME"), CONST_STR_LEN("http"));
	}

	{
		GList *i;

		for (i = vr->request.headers->entries.head; NULL != i; i = i->next) {
			liHttpHeader *h = (liHttpHeader*) i->data;
			const GString hkey = li_const_gstring(h->data->str, h->keylen);
			g_string_truncate(tmp, 0);
			if (!li_strncase_equal(&hkey, CONST_STR_LEN("CONTENT-TYPE"))) {
				g_string_append_len(tmp, CONST_STR_LEN("HTTP_"));
			}
			g_string_append_len(tmp, h->data->str, h->keylen);
			cgi_fix_header_name(tmp);

			add_env_var(envdup, callback, param, GSTR_LEN(tmp), h->data->str + h->keylen+2, h->data->len - (h->keylen+2));
		}
	}

	{
		GHashTableIter i;
		gpointer key, val;

		g_hash_table_iter_init(&i, envdup->table);
		while (g_hash_table_iter_next(&i, &key, &val)) {
			callback(param, GSTR_LEN((GString*) key), GSTR_LEN((GString*) val));
		}
	}

	li_environment_dup_free(envdup);
}

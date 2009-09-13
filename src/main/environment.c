
#include <lighttpd/environment.h>

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
	const GString skey = { (gchar*) key, keylen, 0 }; /* fake a constant GString */
	g_hash_table_remove(env->table, &skey);
}

GString* li_environment_get(liEnvironment *env, const gchar *key, size_t keylen) {
	const GString skey = { (gchar*) key, keylen, 0 }; /* fake a constant GString */
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
	const GString skey = { (gchar*) key, keylen, 0 }; /* fake a constant GString */
	GString *sval = (GString*) g_hash_table_lookup(envdup->table, &skey);
	if (sval) g_hash_table_remove(envdup->table, &skey);
	return sval;
}


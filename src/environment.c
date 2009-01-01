
#include <lighttpd/base.h>

static void _hash_free_gstring(gpointer data) {
	g_string_free((GString*) data, TRUE);
}

void environment_init(environment *env) {
	env->table = g_hash_table_new_full(
		(GHashFunc) g_string_hash, (GEqualFunc) g_string_equal,
		_hash_free_gstring, _hash_free_gstring);
}

void environment_reset(environment *env) {
	g_hash_table_remove_all(env->table);
}

void environment_clear(environment *env) {
	g_hash_table_destroy(env->table);
}

void environment_set(environment *env, const gchar *key, size_t keylen, const gchar *val, size_t valuelen) {
	GString *skey = g_string_new_len(key, keylen);
	GString *sval = g_string_new_len(val, valuelen);
	g_hash_table_insert(env->table, skey, sval);
}

void environment_insert(environment *env, const gchar *key, size_t keylen, const gchar *val, size_t valuelen) {
	GString *sval = environment_get(env, key, keylen), *skey;
	if (!sval) {
		skey = g_string_new_len(key, keylen);
		sval = g_string_new_len(val, valuelen);
		g_hash_table_insert(env->table, skey, sval);
	}
}

void environment_remove(environment *env, const gchar *key, size_t keylen) {
	const GString skey = { (gchar*) key, keylen, 0 }; /* fake a constant GString */
	g_hash_table_remove(env->table, &skey);
}

GString* environment_get(environment *env, const gchar *key, size_t keylen) {
	const GString skey = { (gchar*) key, keylen, 0 }; /* fake a constant GString */
	return (GString*) g_hash_table_lookup(env->table, &skey);
}

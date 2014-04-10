#ifndef _LIGHTTPD_SSL_SESSION_DB_H_
#define _LIGHTTPD_SSL_SESSION_DB_H_

#include <lighttpd/base.h>

typedef struct liSSLSessionDBKey liSSLSessionDBKey;
struct liSSLSessionDBKey {
	GList keys_link;
	size_t size;
	unsigned char data[];
};

typedef struct liSSLSessionDBData liSSLSessionDBData;
struct liSSLSessionDBData {
	gint refcount;
	size_t size;
	unsigned char data[];
};

typedef struct liSSLSessionDB liSSLSessionDB;
struct liSSLSessionDB {
	size_t max_entries;
	GQueue keys; /* move recently added/used entries to end */
	GMutex *mutex;
	GHashTable *db; /* liSSLSessionDBData -> liSSLSessionDBData */
};

INLINE void li_ssl_session_db_data_release(liSSLSessionDBData *d) {
	if (NULL == d) return;
	LI_FORCE_ASSERT(g_atomic_int_get(&d->refcount) > 0);
	if (g_atomic_int_dec_and_test(&d->refcount)) {
		g_slice_free1(d->size + sizeof(liSSLSessionDBData), d);
	}
}

INLINE void li_ssl_session_db_data_free_cb(gpointer data) {
	li_ssl_session_db_data_release(data);
}

INLINE liSSLSessionDBData* li_ssl_session_db_data_new(const unsigned char *data, size_t size) {
	liSSLSessionDBData *d = g_slice_alloc0(size + sizeof(liSSLSessionDBData));
	d->refcount = 1;
	d->size = size;
	memcpy(d->data, data, size);
	return d;
}

INLINE void li_ssl_session_db_key_free_cb(gpointer data) {
	liSSLSessionDBKey *d = data;
	liSSLSessionDB *sdb;
	if (NULL == d) return;
	sdb = d->keys_link.data;
	if (NULL != sdb) g_queue_unlink(&sdb->keys, &d->keys_link);
	g_slice_free1(d->size + sizeof(liSSLSessionDBKey), d);
}

INLINE guint li_ssl_session_db_key_hash(gconstpointer data) {
	const liSSLSessionDBKey *d = data;
	const GString s = li_const_gstring((const gchar*)d->data, d->size);
	return g_string_hash(&s);
}

INLINE gboolean li_ssl_session_db_key_equal(gconstpointer a, gconstpointer b) {
	const liSSLSessionDBKey *da = a, *db = b;
	if (da->size != db->size) return FALSE;
	return 0 == memcmp(da->data, db->data, da->size);
}

INLINE liSSLSessionDBKey* li_ssl_session_db_key_new(const unsigned char *data, size_t size) {
	liSSLSessionDBKey *d = g_slice_alloc0(size + sizeof(liSSLSessionDBKey));
	d->size = size;
	memcpy(d->data, data, size);
	return d;
}


INLINE liSSLSessionDB* li_ssl_session_db_new(size_t max_entries) {
	liSSLSessionDB *sdb = g_slice_new0(liSSLSessionDB);
	sdb->max_entries = max_entries;
	sdb->mutex = g_mutex_new();
	sdb->db = g_hash_table_new_full(li_ssl_session_db_key_hash, li_ssl_session_db_key_equal,
		li_ssl_session_db_key_free_cb, li_ssl_session_db_data_free_cb);
	return sdb;
}

INLINE void li_ssl_session_db_free(liSSLSessionDB* sdb) {
	if (NULL == sdb) return;
	g_mutex_free(sdb->mutex);
	sdb->mutex = NULL;
	g_hash_table_destroy(sdb->db);
	sdb->db = NULL;
	g_slice_free(liSSLSessionDB, sdb);
}

INLINE void li_ssl_session_db_store(liSSLSessionDB *sdb, const unsigned char *key, size_t keylen, const unsigned char *value, size_t valuelen) {
	liSSLSessionDBData *dvalue = li_ssl_session_db_data_new(value, valuelen);
	liSSLSessionDBKey *dkey = li_ssl_session_db_key_new(key, keylen);
	g_mutex_lock(sdb->mutex);
		dkey->keys_link.data = sdb;
		g_queue_push_tail_link(&sdb->keys, &dkey->keys_link);
		g_hash_table_replace(sdb->db, dkey, dvalue);
		while (sdb->keys.length > sdb->max_entries) {
			liSSLSessionDBKey *purge_key = LI_CONTAINER_OF(sdb->keys.head, liSSLSessionDBKey, keys_link);
			g_hash_table_remove(sdb->db, purge_key);
		}
	g_mutex_unlock(sdb->mutex);
}

INLINE liSSLSessionDBData* li_ssl_session_db_lookup(liSSLSessionDB *sdb, const unsigned char *key, size_t keylen) {
	liSSLSessionDBData *dvalue = NULL;
	liSSLSessionDBKey *dkey = li_ssl_session_db_key_new(key, keylen);
	gpointer orig_key, value;

	g_mutex_lock(sdb->mutex);
		if (g_hash_table_lookup_extended(sdb->db, dkey, &orig_key, &value)) {
			liSSLSessionDBKey *k = orig_key;
			g_queue_unlink(&sdb->keys, &k->keys_link);
			g_queue_push_tail_link(&sdb->keys, &k->keys_link);

			dvalue = value;
			LI_FORCE_ASSERT(g_atomic_int_get(&dvalue->refcount) > 0);
			g_atomic_int_inc(&dvalue->refcount);
		}
	g_mutex_unlock(sdb->mutex);
	li_ssl_session_db_key_free_cb(dkey);
	return dvalue;
}

INLINE void li_ssl_session_db_remove(liSSLSessionDB *sdb, const unsigned char *key, size_t keylen) {
	liSSLSessionDBKey *dkey = li_ssl_session_db_key_new(key, keylen);
	g_mutex_lock(sdb->mutex);
		g_hash_table_remove(sdb->db, dkey);
	g_mutex_unlock(sdb->mutex);
	li_ssl_session_db_key_free_cb(dkey);
}

#endif

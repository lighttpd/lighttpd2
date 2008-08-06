
#include "http_headers.h"

static void _string_free(gpointer p) {
	g_string_free((GString*) p, TRUE);
}

static void _string_queue_string_free(gpointer data, gpointer userdata) {
	UNUSED(userdata);
	g_string_free((GString*) data, TRUE);
}

static void _http_header_free(gpointer p) {
	http_header *h = (http_header*) p;
	g_queue_foreach(&h->values, _string_queue_string_free, NULL);
	g_queue_clear(&h->values);
	g_string_free(h->key, TRUE);
	g_slice_free(http_header, h);
}

static http_header* _http_header_new(const gchar *key, size_t keylen) {
	http_header *h = g_slice_new0(http_header);
	g_queue_init(&h->values);
	h->key = g_string_new_len(key, keylen);
	return h;
}

http_headers* http_headers_new() {
	http_headers* headers = g_slice_new0(http_headers);
	headers->table = g_hash_table_new_full(
		(GHashFunc) g_string_hash, (GEqualFunc) g_string_equal,
		_string_free, _http_header_free);
	return headers;
}

void http_headers_reset(http_headers* headers) {
	g_hash_table_remove_all(headers->table);
}

void http_headers_free(http_headers* headers) {
	if (!headers) return;
	g_hash_table_destroy(headers->table);
	g_slice_free(http_headers, headers);
}

/* Just insert the header (using lokey)
 */
static void header_insert(http_headers *headers, GString *lokey,
		const gchar *key, size_t keylen, const gchar *value, size_t valuelen) {
	http_header *h = _http_header_new(key, keylen);
	g_queue_push_tail(&h->values, g_string_new_len(value, valuelen));

	g_hash_table_insert(headers->table, lokey, h);
}

/** If header does not exist, just insert normal header. If it exists, append (", %s", value) */
void http_header_append(http_headers *headers, const gchar *key, size_t keylen, const gchar *value, size_t valuelen) {
	GString *lokey, *tval;
	http_header *h;

	lokey = g_string_new_len(key, keylen);
	g_string_ascii_down(lokey);
	h = (http_header*) g_hash_table_lookup(headers->table, lokey);
	if (NULL == h) {
		header_insert(headers, lokey, key, keylen, value, valuelen);
	} else if (NULL == (tval = g_queue_peek_tail(&h->values))) {
		g_string_free(lokey, TRUE);
		g_queue_push_tail(&h->values, g_string_new_len(value, valuelen));
	} else {
		g_string_free(lokey, TRUE);
		g_string_append_len(tval, ", ", 2);
		g_string_append_len(tval, value, valuelen);
	}
}

/** If header does not exist, just insert normal header. If it exists, append ("\r\n%s: %s", key, value) */
void http_header_insert(http_headers *headers, const gchar *key, size_t keylen, const gchar *value, size_t valuelen) {
	GString *lokey;
	http_header *h;

	lokey = g_string_new_len(key, keylen);
	g_string_ascii_down(lokey);
	h = (http_header*) g_hash_table_lookup(headers->table, lokey);
	if (NULL == h) {
		header_insert(headers, lokey, key, keylen, value, valuelen);
	} else {
		g_string_free(lokey, TRUE);
		g_queue_push_tail(&h->values, g_string_new_len(value, valuelen));
	}
}

/** If header does not exist, just insert normal header. If it exists, overwrite the value */
void http_header_overwrite(http_headers *headers, const gchar *key, size_t keylen, const gchar *value, size_t valuelen) {
	GString *lokey;
	http_header *h;

	lokey = g_string_new_len(key, keylen);
	g_string_ascii_down(lokey);
	h = (http_header*) g_hash_table_lookup(headers->table, lokey);
	if (NULL == h) {
		header_insert(headers, lokey, key, keylen, value, valuelen);
	} else {
		g_string_free(lokey, TRUE);
		g_string_truncate(h->key, 0);
		g_string_append_len(h->key, key, keylen);
		/* kill old headers */
		g_queue_foreach(&h->values, _string_queue_string_free, NULL);
		/* new header */
		g_queue_push_tail(&h->values, g_string_new_len(value, valuelen));
	}
}

gboolean http_header_remove(http_headers *headers, const gchar *key, size_t keylen) {
	GString *lokey;
	gboolean res;

	lokey = g_string_new_len(key, keylen);
	g_string_ascii_down(lokey);
	res = g_hash_table_remove(headers->table, lokey);
	g_string_free(lokey, TRUE);
	return res;
}

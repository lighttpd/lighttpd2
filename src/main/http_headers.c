
#include <lighttpd/base.h>

static void _http_header_free(gpointer p) {
	liHttpHeader *h = (liHttpHeader*) p;
	g_string_free(h->data, TRUE);
	g_slice_free(liHttpHeader, h);
}

static liHttpHeader* _http_header_new(const gchar *key, size_t keylen, const gchar *val, size_t valuelen) {
	liHttpHeader *h = g_slice_new0(liHttpHeader);
	gchar *s;
	h->data = g_string_sized_new(keylen + valuelen + 2);
	g_string_set_size(h->data, keylen + valuelen + 2);
	h->keylen = keylen;
	s = h->data->str;
	memcpy(s, key, keylen);
	s += keylen;
	memcpy(s, ": ", 2);
	s += 2;
	memcpy(s, val, valuelen);
	return h;
}

static void _header_queue_free(gpointer data, gpointer userdata) {
	UNUSED(userdata);
	_http_header_free((liHttpHeader*) data);
}

liHttpHeaders* li_http_headers_new() {
	liHttpHeaders* headers = g_slice_new0(liHttpHeaders);
	g_queue_init(&headers->entries);
	return headers;
}

void li_http_headers_reset(liHttpHeaders* headers) {
	g_queue_foreach(&headers->entries, _header_queue_free, NULL);
	g_queue_clear(&headers->entries);
}

void li_http_headers_free(liHttpHeaders* headers) {
	if (!headers) return;
	g_queue_foreach(&headers->entries, _header_queue_free, NULL);
	g_queue_clear(&headers->entries);
	g_slice_free(liHttpHeaders, headers);
}

/** just insert normal header, allow duplicates */
void li_http_header_insert(liHttpHeaders *headers, const gchar *key, size_t keylen, const gchar *val, size_t valuelen) {
	liHttpHeader *h = _http_header_new(key, keylen, val, valuelen);
	g_queue_push_tail(&headers->entries, h);
}

GList* li_http_header_find_first(liHttpHeaders *headers, const gchar *key, size_t keylen) {
	liHttpHeader *h;
	GList *l;

	for (l = g_queue_peek_head_link(&headers->entries); l; l = g_list_next(l)) {
		h = (liHttpHeader*) l->data;
		if (h->keylen == keylen && 0 == g_ascii_strncasecmp(key, h->data->str, keylen)) return l;
	}
	return NULL;
}

GList* li_http_header_find_next(GList *l, const gchar *key, size_t keylen) {
	liHttpHeader *h;

	for (l = g_list_next(l); l; l = g_list_next(l)) {
		h = (liHttpHeader*) l->data;
		if (h->keylen == keylen && 0 == g_ascii_strncasecmp(key, h->data->str, keylen)) return l;
	}
	return NULL;
}

GList* li_http_header_find_last(liHttpHeaders *headers, const gchar *key, size_t keylen) {
	liHttpHeader *h;
	GList *l;

	for (l = g_queue_peek_tail_link(&headers->entries); l; l = g_list_previous(l)) {
		h = (liHttpHeader*) l->data;
		if (h->keylen == keylen && 0 == g_ascii_strncasecmp(key, h->data->str, keylen)) return l;
	}
	return NULL;
}

/** If header does not exist, just insert normal header. If it exists, append (", %s", value) to the last inserted one */
void li_http_header_append(liHttpHeaders *headers, const gchar *key, size_t keylen, const gchar *val, size_t valuelen) {
	GList *l;
	liHttpHeader *h;

	l = li_http_header_find_last(headers, key, keylen);
	if (NULL == l) {
		li_http_header_insert(headers, key, keylen, val, valuelen);
	} else {
		gsize oldlen;
		gchar *s;
		h = (liHttpHeader*) l->data;
		oldlen = h->data->len;
		g_string_set_size(h->data, oldlen + 2 + valuelen);
		s = h->data->str + oldlen;
		memcpy(s, ", ", 2);
		memcpy(s+2, val, valuelen);
	}
}

/** If header does not exist, just insert normal header. If it exists, overwrite the last occurrence */
void li_http_header_overwrite(liHttpHeaders *headers, const gchar *key, size_t keylen, const gchar *val, size_t valuelen) {
	GList *l;
	liHttpHeader *h;

	l = li_http_header_find_last(headers, key, keylen);
	if (NULL == l) {
		li_http_header_insert(headers, key, keylen, val, valuelen);
	} else {
		h = (liHttpHeader*) l->data;
		g_string_set_size(h->data, keylen + 2 + valuelen);
		/* only overwrite value */
		memcpy(h->data->str + keylen + 2, val, valuelen);
	}
}

void li_http_header_remove_link(liHttpHeaders *headers, GList *l) {
	_http_header_free(l->data);
	g_queue_delete_link(&headers->entries, l);
}

gboolean li_http_header_remove(liHttpHeaders *headers, const gchar *key, size_t keylen) {
	GList *l, *lp = NULL;
	gboolean res = FALSE;

	for (l = li_http_header_find_first(headers, key, keylen); l; l = li_http_header_find_next(l, key, keylen)) {
		if (lp) {
			li_http_header_remove_link(headers, lp);
			res = TRUE;
			lp = NULL;
		}
		lp = l;
	}
	if (lp) {
		li_http_header_remove_link(headers, lp);
		res = TRUE;
		lp = NULL;
	}
	return res;
}

liHttpHeader* li_http_header_lookup(liHttpHeaders *headers, const gchar *key, size_t keylen) {
	GList *l;

	l = li_http_header_find_last(headers, key, keylen);
	return NULL == l ? NULL : (liHttpHeader*) l->data;
}

gboolean li_http_header_is(liHttpHeaders *headers, const gchar *key, size_t keylen, const gchar *val, size_t valuelen) {
	GList *l;
	UNUSED(valuelen);

	for (l = li_http_header_find_first(headers, key, keylen); l; l = li_http_header_find_next(l, key, keylen)) {
		liHttpHeader *h = (liHttpHeader*) l->data;
		if (h->data->len - (h->keylen + 2) != valuelen) continue;
		if (0 == g_ascii_strcasecmp( &h->data->str[h->keylen+2], val )) return TRUE;
	}
	return FALSE;
}

void li_http_header_get_all(GString *dest, liHttpHeaders *headers, const gchar *key, size_t keylen) {
	GList *l;
	g_string_truncate(dest, 0);

	for (l = li_http_header_find_first(headers, key, keylen); l; l = li_http_header_find_next(l, key, keylen)) {
		liHttpHeader *h = (liHttpHeader*) l->data;
		if (dest->len) g_string_append_len(dest, CONST_STR_LEN(", "));
		g_string_append_len(dest, &h->data->str[h->keylen+2], h->data->len - (h->keylen + 2));
	}
}

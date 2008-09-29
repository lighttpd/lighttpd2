#ifndef _LIGHTTPD_HTTP_HEADERS_H_
#define _LIGHTTPD_HTTP_HEADERS_H_

struct http_header;
typedef struct http_header http_header;

struct http_headers;
typedef struct http_headers http_headers;

#include "settings.h"

#define HEADER_VALUE(h) \
	(&(h)->data->str[h->keylen + 2])

#define HEADER_VALUE_LEN(h) \
	(&(h)->data->str[h->keylen + 2]), ((h)->data->len - (h->keylen + 2))

struct http_header {
	guint keylen;     /** length of "headername" in data */
	GString *data;    /** "headername: value" */
};

struct http_headers {
	GQueue entries;
};

/* strings alweays get copied, so you should free key and value yourself */

LI_API http_headers* http_headers_new();
LI_API void http_headers_reset(http_headers* headers);
LI_API void http_headers_free(http_headers* headers);

/** If header does not exist, just insert normal header. If it exists, append (", %s", value) */
LI_API void http_header_append(http_headers *headers, const gchar *key, size_t keylen, const gchar *value, size_t valuelen);
/** If header does not exist, just insert normal header. If it exists, append ("\r\n%s: %s", key, value) */
LI_API void http_header_insert(http_headers *headers, const gchar *key, size_t keylen, const gchar *value, size_t valuelen);
/** If header does not exist, just insert normal header. If it exists, overwrite the value */
LI_API void http_header_overwrite(http_headers *headers, const gchar *key, size_t keylen, const gchar *value, size_t valuelen);
LI_API gboolean http_header_remove(http_headers *headers, const gchar *key, size_t keylen);
LI_API void http_header_remove_link(http_headers *headers, GList *l);

LI_API http_header* http_header_lookup(http_headers *headers, const gchar *key, size_t keylen);

LI_API GList* http_header_find_first(http_headers *headers, const gchar *key, size_t keylen);
LI_API GList* http_header_find_next(GList *l, const gchar *key, size_t keylen);
LI_API GList* http_header_find_last(http_headers *headers, const gchar *key, size_t keylen);

/** Use lowercase keys! values are compared case-insensitive */
LI_API gboolean http_header_is(http_headers *headers, const gchar *key, size_t keylen, const gchar *value, size_t valuelen);

/** concats all headers with key with ', ' - empty if no header exists - use lowercase key*/
LI_API void http_header_get_fast(GString *dest, http_headers *headers, const gchar *key, size_t keylen);

INLINE gboolean http_header_key_is(http_header *h, const gchar *key, size_t keylen) {
	return (h->keylen == keylen && 0 == g_ascii_strncasecmp(key, h->data->str, keylen));
}

#endif

#ifndef _LIGHTTPD_HTTP_HEADERS_H_
#define _LIGHTTPD_HTTP_HEADERS_H_

struct http_header;
typedef struct http_header http_header;

struct http_headers;
typedef struct http_headers http_headers;

#include "settings.h"

struct http_header {
	GString *key;
	GQueue values; /**< queue of GString* */
};

struct http_headers {
	/** keys are lowercase header name (GString*), values are http_header* */
	GHashTable *table;
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

#endif

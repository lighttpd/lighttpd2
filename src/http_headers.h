#ifndef _LIGHTTPD_HTTP_HEADERS_H_
#define _LIGHTTPD_HTTP_HEADERS_H_

struct http_headers;
typedef struct http_headers http_headers;

#include "settings.h"

struct http_headers {
	/** keys are lowercase header name, values contain the complete header */
	GHashTable *table;
};

/* strings alweays get copied, so you should free key and value yourself */

LI_API http_headers* http_headers_new();
LI_API void http_headers_free(http_headers* headers);

/** If header does not exist, just insert normal header. If it exists, append (", %s", value) */
LI_API void http_header_append(http_headers *headers, GString *key, GString *value);
/** If header does not exist, just insert normal header. If it exists, append ("\r\n%s: %s", key, value) */
LI_API void http_header_insert(http_headers *headers, GString *key, GString *value);
/** If header does not exist, just insert normal header. If it exists, overwrite the value */
LI_API void http_header_overwrite(http_headers *headers, GString *key, GString *value);
LI_API gboolean http_header_remove(http_headers *headers, GString *key);

#endif

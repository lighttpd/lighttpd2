#ifndef _LIGHTTPD_ENCODING_H_
#define _LIGHTTPD_ENCODING_H_

#ifndef _LIGHTTPD_BASE_H_
#error Please include <lighttpd/base.h> instead of this file
#endif

typedef enum {
	ENCODING_HEX,  /* a => 61 */
	ENCODING_HTML, /* HTML special chars. & => &amp; e.g. */
	ENCODING_URI   /* relative URI */
} encoding_t;


/* encodes special characters in a string and returns the new string */
GString *string_encode(const gchar *str, GString *dest, encoding_t encoding);

#endif
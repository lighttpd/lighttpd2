#ifndef _LIGHTTPD_ENCODING_H_
#define _LIGHTTPD_ENCODING_H_

#include <lighttpd/settings.h>

typedef enum {
	LI_ENCODING_HEX,  /* a => 61 */
	LI_ENCODING_HTML, /* HTML special chars. & => &amp; e.g. */
	LI_ENCODING_URI   /* relative URI */
} liEncoding;


/* encodes special characters in a string and returns the new string */
LI_API GString *li_string_encode_append(const gchar *str, GString *dest, liEncoding encoding);
LI_API GString *li_string_encode(const gchar *str, GString *dest, liEncoding encoding);

#endif

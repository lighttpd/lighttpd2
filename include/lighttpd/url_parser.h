#ifndef _LIGHTTPD_URL_PARSER_H_
#define _LIGHTTPD_URL_PARSER_H_

#include <lighttpd/base.h>

/* parses uri->raw into all components, which have to be reset/initialized before */
LI_API gboolean li_parse_raw_url(liRequestUri *uri);

/* parse input into uri->path, uri->raw_path and uri->query, which get truncated before.
 * also decodes and simplifies path on success
 */
LI_API gboolean li_parse_raw_path(liRequestUri *uri, GString *input);

LI_API gboolean li_parse_hostname(liRequestUri *uri);

#endif

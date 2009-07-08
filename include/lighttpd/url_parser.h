#ifndef _LIGHTTPD_URL_PARSER_H_
#define _LIGHTTPD_URL_PARSER_H_

#include <lighttpd/base.h>

LI_API gboolean parse_raw_url(liRequestUri *uri);
LI_API gboolean parse_hostname(liRequestUri *uri);

#endif

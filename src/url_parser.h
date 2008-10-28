#ifndef _LIGHTTPD_URL_PARSER_H_
#define _LIGHTTPD_URL_PARSER_H_

#include "base.h"

LI_API gboolean parse_raw_url(request_uri *uri);
LI_API gboolean parse_hostname(request_uri *uri);

#endif

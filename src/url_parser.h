#ifndef _LIGHTTPD_URL_PARSER_H_
#define _LIGHTTPD_URL_PARSER_H_

#include "settings.h"
#include "request.h"

LI_API gboolean parse_raw_url(request_uri *uri);
LI_API gboolean parse_authority(request_uri *uri);

#endif

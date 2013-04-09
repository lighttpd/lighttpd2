#ifndef _LIGHTTPD_STREAM_HTTP_RESPONSE_H_
#define _LIGHTTPD_STREAM_HTTP_RESPONSE_H_

#include <lighttpd/base.h>

LI_API liStream* li_stream_http_response_handle(liStream *http_in, liVRequest *vr, gboolean accept_cgi, gboolean accept_nph);

#endif

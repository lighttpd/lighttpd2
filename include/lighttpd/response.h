#ifndef _LIGHTTPD_RESPONSE_H_
#define _LIGHTTPD_RESPONSE_H_

#ifndef _LIGHTTPD_BASE_H_
#error Please include <lighttpd/base.h> instead of this file
#endif

struct liResponse {
	liHttpHeaders *headers;
	gint http_status;
	liTransferEncoding transfer_encoding;
};

LI_API void li_response_init(liResponse *resp);
LI_API void li_response_reset(liResponse *resp);
LI_API void li_response_clear(liResponse *resp);

LI_API void li_response_send_headers(liVRequest *vr, liChunkQueue *raw_out, liChunkQueue *response_body);

#endif

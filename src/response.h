#ifndef _LIGHTTPD_RESPONSE_H_
#define _LIGHTTPD_RESPONSE_H_

struct response;
typedef struct response response;

#include "http_headers.h"

struct response {
	http_headers *headers;
	gint http_status;
	transfer_encoding_t transfer_encoding;
};

LI_API void response_init(response *resp);
LI_API void response_reset(response *resp);
LI_API void response_clear(response *resp);

LI_API void response_send_headers(connection *con);
LI_API void response_send_error_page(connection *con);

#endif

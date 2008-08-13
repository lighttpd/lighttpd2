#ifndef _LIGHTTPD_FILTER_CHUNKED_H_
#define _LIGHTTPD_FILTER_CHUNKED_H_

#include "base.h"

LI_API handler_t filter_chunked_encode(server *srv, connection *con, chunkqueue *out, chunkqueue *in);
LI_API handler_t filter_chunked_decode(server *srv, connection *con, chunkqueue *out, chunkqueue *in);

#endif

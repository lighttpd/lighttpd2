#ifndef _LIGHTTPD_FILTER_CHUNKED_H_
#define _LIGHTTPD_FILTER_CHUNKED_H_

#include <lighttpd/base.h>

LI_API liHandlerResult filter_chunked_encode(liConnection *con, liChunkQueue *out, liChunkQueue *in);
LI_API liHandlerResult filter_chunked_decode(liConnection *con, liChunkQueue *out, liChunkQueue *in);

#endif

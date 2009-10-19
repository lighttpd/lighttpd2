#ifndef _LIGHTTPD_FILTER_CHUNKED_H_
#define _LIGHTTPD_FILTER_CHUNKED_H_

#include <lighttpd/base.h>

/* initialize with zero */
typedef struct {
	int parse_state;
	goffset cur_chunklen;
} liFilterDecodeState;

LI_API liHandlerResult li_filter_chunked_encode(liVRequest *vr, liChunkQueue *out, liChunkQueue *in);
LI_API liHandlerResult li_filter_chunked_decode(liVRequest *vr, liChunkQueue *out, liChunkQueue *in, liFilterDecodeState *state);

#endif

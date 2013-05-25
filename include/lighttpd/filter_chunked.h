#ifndef _LIGHTTPD_FILTER_CHUNKED_H_
#define _LIGHTTPD_FILTER_CHUNKED_H_

#ifndef _LIGHTTPD_BASE_H_
#error Please include <lighttpd/base.h> instead of this file
#endif

/* initialize with zero */
typedef struct {
	int parse_state;
	goffset cur_chunklen;
} liFilterChunkedDecodeState;

LI_API liHandlerResult li_filter_chunked_encode(liVRequest *vr, liChunkQueue *out, liChunkQueue *in);
LI_API gboolean li_filter_chunked_decode(liVRequest *vr, liChunkQueue *out, liChunkQueue *in, liFilterChunkedDecodeState *state);

#endif

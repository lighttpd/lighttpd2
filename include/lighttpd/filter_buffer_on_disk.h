#ifndef _LIGHTTPD_FILTER_BUFFER_ON_DISK_H_
#define _LIGHTTPD_FILTER_BUFFER_ON_DISK_H_

#include <lighttpd/base.h>

/* initialize with zero */
typedef struct {
	/* internal state */
	liChunkFile *tempfile;
	goffset flush_pos, write_pos;

	/* config */
	goffset flush_limit; /* -1: wait for end-of-stream, n >= 0: if more than n bytes have been written, the next part of the file gets forwarded to out */
	gboolean split_on_file_chunks; /* start a new file on FILE_CHUNK (those are not written to the file) */
} liFilterBufferOnDiskState;

LI_API liHandlerResult li_filter_buffer_on_disk(liVRequest *vr, liChunkQueue *out, liChunkQueue *in, liFilterBufferOnDiskState *state);
LI_API void li_filter_buffer_on_disk_reset(liFilterBufferOnDiskState *state);


#endif

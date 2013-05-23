#ifndef _LIGHTTPD_FILTER_BUFFER_ON_DISK_H_
#define _LIGHTTPD_FILTER_BUFFER_ON_DISK_H_

#include <lighttpd/base.h>

/* flush_limit: -1: wait for end-of-stream, n >= 0: if more than n bytes have been written, the next part of the file gets forwarded to out */
/* split_on_file_chunks: start a new file on FILE_CHUNK (those are not written to the file) */
LI_API liStream* li_filter_buffer_on_disk(liVRequest *vr, goffset flush_limit, gboolean split_on_file_chunks);
LI_API void li_filter_buffer_on_disk_stop(liStream *stream);

#endif

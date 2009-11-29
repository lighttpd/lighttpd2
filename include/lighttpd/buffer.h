#ifndef _LIGHTTPD_BUFFER_H_
#define _LIGHTTPD_BUFFER_H_

#include <lighttpd/settings.h>

#include <lighttpd/mempool.h>

typedef struct liBuffer liBuffer;
struct liBuffer {
	gchar *addr;
	gsize alloc_size;
	gsize used;
	gint refcount;
	mempool_ptr mptr;
};

/* shared buffer; free memory after last reference is released */
LI_API liBuffer* li_buffer_new(gsize max_size);
LI_API void li_buffer_acquire(liBuffer *buf);
LI_API void li_buffer_release(liBuffer *buf);

#endif

#ifndef _LIGHTTPD_MEMPOOL_H_
#define _LIGHTTPD_MEMPOOL_H_

#include <lighttpd/settings.h>

typedef struct mempool_ptr mempool_ptr;
struct mempool_ptr {
	void *priv_data; /* private data for internal management */
	void *data; /* real pointer (result of alloc) */
};

LI_API gsize mempool_align_page_size(gsize size);
LI_API mempool_ptr mempool_alloc(gsize size);

/* you cannot release parts from an allocated chunk; so you _have_ to remember the size from the alloc */
LI_API void mempool_free(mempool_ptr ptr, gsize size);

LI_API void mempool_cleanup();

#endif

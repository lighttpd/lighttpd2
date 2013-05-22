#ifndef _LIGHTTPD_MEMPOOL_H_
#define _LIGHTTPD_MEMPOOL_H_

#include <lighttpd/settings.h>

typedef struct liMempoolPtr liMempoolPtr;
struct liMempoolPtr {
	void *priv_data; /* private data for internal management */
	void *data; /* real pointer (result of alloc) */
};

LI_API gsize li_mempool_align_page_size(gsize size);
LI_API liMempoolPtr li_mempool_alloc(gsize size);

/* you cannot release parts from an allocated chunk; so you _have_ to remember the size from the alloc */
LI_API void li_mempool_free(liMempoolPtr ptr, gsize size);

LI_API void li_mempool_cleanup();

#endif

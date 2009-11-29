
#include <lighttpd/buffer.h>

static void _buffer_init(liBuffer *buf, gsize alloc_size) {
	buf->alloc_size = alloc_size;
	buf->used = 0;
	buf->mptr = mempool_alloc(alloc_size);
	buf->addr = buf->mptr.data;
}

static void _buffer_destroy(liBuffer *buf) {
	if (!buf || NULL == buf->addr) return;
	mempool_free(buf->mptr, buf->alloc_size);
	buf->addr = NULL;
	buf->mptr.data = NULL; buf->mptr.priv_data = NULL;
	buf->used = buf->alloc_size = 0;
}


liBuffer* li_buffer_new(gsize max_size) {
	liBuffer *buf = g_slice_new0(liBuffer);
	_buffer_init(buf, mempool_align_page_size(max_size));
	buf->refcount = 1;
	return buf;
}

void li_buffer_release(liBuffer *buf) {
	if (!buf) return;
	assert(g_atomic_int_get(&buf->refcount) > 0);
	if (g_atomic_int_dec_and_test(&buf->refcount)) {
		_buffer_destroy(buf);
		g_slice_free(liBuffer, buf);
	}
}

void li_buffer_acquire(liBuffer *buf) {
	assert(g_atomic_int_get(&buf->refcount) > 0);
	g_atomic_int_inc(&buf->refcount);
}

#ifndef _LIGHTTPD_CHUNK_H_
#define _LIGHTTPD_CHUNK_H_

#ifndef _LIGHTTPD_BASE_H_
#error Please include <lighttpd/base.h> instead of this file
#endif

/* Open a file only once, so it shouldn't get lost;
 * as a file may get split into many chunks, we
 * use this struct to keep track of the usage
 */
struct liChunkFile {
	gint refcount;

	GString *name; /* name of the file */
	int fd;
	gboolean is_temp; /* file is temporary and will be deleted on cleanup */
};

struct liChunk {
	enum { UNUSED_CHUNK, STRING_CHUNK, MEM_CHUNK, FILE_CHUNK } type;

	goffset offset;
	/* if type == FILE_CHUNK and mem != NULL,
	 * mem contains the data [file.mmap.offset .. file.mmap.offset + file.mmap.length)
	 * from the file, and file.mmap.start is NULL as mmap failed and read(...) was used.
	 */
	GString *str;
	GByteArray *mem;

	struct {
		liChunkFile *file;
		off_t start; /* starting offset in the file */
		off_t length; /* octets to send from the starting offset */

		struct {
			char   *data; /* the pointer of the mmap'ed area */
			size_t length; /* size of the mmap'ed area */
			off_t  offset; /* start is <n> octets away from the start of the file */
		} mmap;
	} file;
};

typedef void (*liCQLimitNnotifyCB)(liVRequest *vr, gpointer context, gboolean locked);
struct liCQLimit {
	gint refcount;
	liVRequest *vr;

	goffset limit, current;
	gboolean locked;

	ev_io *io_watcher;

	liCQLimitNnotifyCB notify; /* callback to reactivate input */
	gpointer context;
};

struct liChunkQueue {
/* public */
	gboolean is_closed;
/* read only */
	goffset bytes_in, bytes_out, length, mem_usage;
	liCQLimit *limit; /* limit is the sum of all { c->mem->len | c->type == STRING_CHUNK } */
/* private */
	GQueue *queue;
};

struct liChunkIter {
/* private */
	GList *element;
};

/******************
 *   chunkfile    *
 ******************/

/* open the file cf->name if it is not already opened for reading
 * may return HANDLER_GO_ON, HANDLER_ERROR
 */
LI_API liHandlerResult li_chunkfile_open(liVRequest *vr, liChunkFile *cf);

/******************
 * chunk iterator *
 ******************/

INLINE liChunk* chunkiter_chunk(liChunkIter iter);
INLINE gboolean chunkiter_next(liChunkIter *iter);
INLINE goffset chunkiter_length(liChunkIter iter);

/* get the data from a chunk; easy in case of a STRING_CHUNK,
 * but needs to do io in case of FILE_CHUNK; the data is _not_ marked as "done"
 * may return HANDLER_GO_ON, HANDLER_ERROR
 */
LI_API liHandlerResult li_chunkiter_read(liVRequest *vr, liChunkIter iter, off_t start, off_t length, char **data_start, off_t *data_len);

/* same as li_chunkiter_read, but tries mmap() first and falls back to read();
 * as accessing mmap()-ed areas may result in SIGBUS, you have to handle that signal somehow.
 */
LI_API liHandlerResult li_chunkiter_read_mmap(liVRequest *vr, liChunkIter iter, off_t start, off_t length, char **data_start, off_t *data_len);

/******************
 *     chunk      *
 ******************/

INLINE goffset chunk_length(liChunk *c);

/******************
 *    cqlimit     *
 ******************/

LI_API liCQLimit* li_cqlimit_new(liVRequest *vr);
LI_API void li_cqlimit_reset(liCQLimit *cql);
LI_API void li_cqlimit_acquire(liCQLimit *cql);
LI_API void li_cqlimit_release(liCQLimit *cql);
LI_API void li_cqlimit_set_limit(liCQLimit *cql, goffset limit);

/******************
 *   chunkqueue   *
 ******************/

LI_API liChunkQueue* li_chunkqueue_new();
LI_API void li_chunkqueue_reset(liChunkQueue *cq);
LI_API void li_chunkqueue_free(liChunkQueue *cq);

LI_API void li_chunkqueue_use_limit(liChunkQueue *cq, liVRequest *vr);
LI_API void li_chunkqueue_set_limit(liChunkQueue *cq, liCQLimit* cql);
/* return -1 for unlimited, 0 for full and n > 0 for n bytes free */
LI_API goffset li_chunkqueue_limit_available(liChunkQueue *cq);

 /* pass ownership of str to chunkqueue, do not free/modify it afterwards
  * you may modify the data (not the length) if you are sure it isn't sent before.
  * if the length is NULL, str is destroyed immediately
  */
LI_API void li_chunkqueue_append_string(liChunkQueue *cq, GString *str);

 /* pass ownership of mem to chunkqueue, do not free/modify it afterwards
  * you may modify the data (not the length) if you are sure it isn't sent before.
  * if the length is NULL, mem is destroyed immediately
  */
LI_API void li_chunkqueue_append_bytearr(liChunkQueue *cq, GByteArray *mem);

/* memory gets copied */
LI_API void li_chunkqueue_append_mem(liChunkQueue *cq, const void *mem, gssize len);

/* pass ownership of filename, do not free it */
LI_API void li_chunkqueue_append_file(liChunkQueue *cq, GString *filename, off_t start, off_t length);
/* if you already opened the file, you can pass the fd here - do not close it */
LI_API void li_chunkqueue_append_file_fd(liChunkQueue *cq, GString *filename, off_t start, off_t length, int fd);

/* temp files get deleted after usage */
/* pass ownership of filename, do not free it */
LI_API void li_chunkqueue_append_tempfile(liChunkQueue *cq, GString *filename, off_t start, off_t length);
/* if you already opened the file, you can pass the fd here - do not close it */
LI_API void li_chunkqueue_append_tempfile_fd(liChunkQueue *cq, GString *filename, off_t start, off_t length, int fd);


/* steal up to length bytes from in and put them into out, return number of bytes stolen */
LI_API goffset li_chunkqueue_steal_len(liChunkQueue *out, liChunkQueue *in, goffset length);

/* steal all chunks from in and put them into out, return number of bytes stolen */
LI_API goffset li_chunkqueue_steal_all(liChunkQueue *out, liChunkQueue *in);

/* steal the first chunk from in and append it to out, return number of bytes stolen */
LI_API goffset li_chunkqueue_steal_chunk(liChunkQueue *out, liChunkQueue *in);

/* skip up to length bytes in a chunkqueue, return number of bytes skipped */
LI_API goffset li_chunkqueue_skip(liChunkQueue *cq, goffset length);

/* skip all chunks in a queue (similar to reset, but keeps stats) */
LI_API goffset li_chunkqueue_skip_all(liChunkQueue *cq);

/* if the chunk an iterator refers gets stolen/skipped/...,
 * the iterator isn't valid anymore
 */
INLINE liChunkIter chunkqueue_iter(liChunkQueue *cq);

INLINE liChunk* chunkqueue_first_chunk(liChunkQueue *cq);

LI_API gboolean li_chunkqueue_extract_to(liVRequest *vr, liChunkQueue *cq, goffset len, GString *dest);
LI_API gboolean li_chunkqueue_extract_to_bytearr(liVRequest *vr, liChunkQueue *cq, goffset len, GByteArray *dest);

/********************
 * Inline functions *
 ********************/

INLINE liChunk* chunkiter_chunk(liChunkIter iter) {
	if (!iter.element) return NULL;
	return (liChunk*) iter.element->data;
}

INLINE gboolean chunkiter_next(liChunkIter *iter) {
	if (!iter || !iter->element) return FALSE;
	return NULL != (iter->element = g_list_next(iter->element));
}

INLINE goffset chunkiter_length(liChunkIter iter) {
	return chunk_length(chunkiter_chunk(iter));
}

INLINE goffset chunk_length(liChunk *c) {
	if (!c) return 0;
	switch (c->type) {
	case UNUSED_CHUNK:
		return 0;
	case STRING_CHUNK:
		return c->str->len - c->offset;
	case MEM_CHUNK:
		return c->mem->len - c->offset;
	case FILE_CHUNK:
		return c->file.length - c->offset;
	}
	return 0;
}

INLINE liChunkIter chunkqueue_iter(liChunkQueue *cq) {
	liChunkIter i;
	i.element = g_queue_peek_head_link(cq->queue);
	return i;
}

INLINE liChunk* chunkqueue_first_chunk(liChunkQueue *cq) {
	return (liChunk*) g_queue_peek_head(cq->queue);
}

#endif

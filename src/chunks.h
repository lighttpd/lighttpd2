#ifndef _LIGHTTPD_CHUNK_H_
#define _LIGHTTPD_CHUNK_H_

#include <glib.h>

struct chunkfile;
typedef struct chunkfile chunkfile;

struct chunk;
typedef struct chunk chunk;

struct chunkqueue;
typedef struct chunkqueue chunkqueue;

struct chunkiter;
typedef struct chunkiter chunkiter;

#include "base.h"

/* Open a file only once, so it shouldn't get lost;
 * as a file may get split into many chunks, we
 * use this struct to keep track of the usage
 */
struct chunkfile {
	gint refcount;

	GString *name; /* name of the file */
	int fd;
	gboolean is_temp; /* file is temporary and will be deleted on cleanup */
};

struct chunk {
	enum { UNUSED_CHUNK, MEM_CHUNK, FILE_CHUNK } type;

	goffset offset;
	/* if type == FILE_CHUNK and mem != NULL,
	 * mem contains the data [file.mmap.offset .. file.mmap.offset + file.mmap.length)
	 * from the file, and file.mmap.start is NULL as mmap failed and read(...) was used.
	 */
	GString *mem;

	struct {
		chunkfile *file;
		off_t start; /* starting offset in the file */
		off_t length; /* octets to send from the starting offset */

		struct {
			char   *data; /* the pointer of the mmap'ed area */
			size_t length; /* size of the mmap'ed area */
			off_t  offset; /* start is <n> octets away from the start of the file */
		} mmap;
	} file;
};

struct chunkqueue {
/* public */
	gboolean is_closed;
/* read only */
	goffset bytes_in, bytes_out, length;
/* private */
	GQueue *queue;
};

struct chunkiter {
/* private */
	GList *element;
};

/******************
 *   chunkfile    *
 ******************/

/* open the file cf->name if it is not already opened for reading
 * may return HANDLER_GO_ON, HANDLER_ERROR, HANDLER_WAIT_FOR_FD
 */
handler_t chunkfile_open(server *srv, connection *con, chunkfile *cf);

/******************
 * chunk iterator *
 ******************/

INLINE chunk* chunkiter_chunk(chunkiter iter);
INLINE gboolean chunkiter_next(chunkiter *iter);
INLINE goffset chunkiter_length(chunkiter iter);

/* get the data from a chunk; easy in case of a MEM_CHUNK,
 * but needs to do io in case of FILE_CHUNK; it tries mmap and
 * falls back to read(...)
 * the data is _not_ marked as "done"
 * may return HANDLER_GO_ON, HANDLER_ERROR, HANDLER_WAIT_FOR_FD
 */
handler_t chunkiter_read(server *srv, connection *con, chunkiter iter, off_t start, off_t length, char **data_start, off_t *data_len);

/******************
 *     chunk      *
 ******************/

INLINE goffset chunk_length(chunk *c);

/******************
 *   chunkqueue   *
 ******************/

chunkqueue* chunkqueue_new();
void chunkqueue_reset(chunkqueue *cq);
void chunkqueue_free(chunkqueue *cq);

 /* pass ownership of str to chunkqueue, do not free/modify it afterwards
  * you may modify the data (not the length) if you are sure it isn't sent before.
  */
void chunkqueue_append_string(chunkqueue *cq, GString *str);

/* memory gets copied */
void chunkqueue_append_mem(chunkqueue *cq, void *mem, gssize len);

/* pass ownership of filename, do not free it */
void chunkqueue_append_file(chunkqueue *cq, GString *filename, off_t start, off_t length);
/* if you already opened the file, you can pass the fd here - do not close it */
void chunkqueue_append_file_fd(chunkqueue *cq, GString *filename, off_t start, off_t length, int fd);

/* temp files get deleted after usage */
/* pass ownership of filename, do not free it */
void chunkqueue_append_tempfile(chunkqueue *cq, GString *filename, off_t start, off_t length);
/* if you already opened the file, you can pass the fd here - do not close it */
void chunkqueue_append_tempfile_fd(chunkqueue *cq, GString *filename, off_t start, off_t length, int fd);


/* steal up to length bytes from in and put them into out, return number of bytes stolen */
goffset chunkqueue_steal_len(chunkqueue *out, chunkqueue *in, goffset length);

/* steal all chunks from in and put them into out, return number of bytes stolen */
goffset chunkqueue_steal_all(chunkqueue *out, chunkqueue *in);

/* steal the first chunk from in and append it to out, return number of bytes stolen */
goffset chunkqueue_steal_chunk(chunkqueue *out, chunkqueue *in);

/* skip up to length bytes in a chunkqueue, return number of bytes skipped */
goffset chunkqueue_skip(chunkqueue *cq, goffset length);

/* skip all chunks in a queue (similar to reset, but keeps stats) */
goffset chunkqueue_skip_all(chunkqueue *cq);

/* if the chunk an iterator refers gets stolen/skipped/...,
 * the iterator isn't valid anymore
 */
INLINE chunkiter chunkqueue_iter(chunkqueue *cq);

INLINE chunk* chunkqueue_first_chunk(chunkqueue *cq);

/********************
 * Inline functions *
 ********************/

INLINE chunk* chunkiter_chunk(chunkiter iter) {
	if (!iter.element) return NULL;
	return (chunk*) iter.element->data;
}

INLINE gboolean chunkiter_next(chunkiter *iter) {
	if (!iter || !iter->element) return FALSE;
	return NULL != (iter->element = g_list_next(iter->element));
}

INLINE goffset chunkiter_length(chunkiter iter) {
	return chunk_length(chunkiter_chunk(iter));
}

INLINE goffset chunk_length(chunk *c) {
	if (!c) return 0;
	switch (c->type) {
	case UNUSED_CHUNK:
		return 0;
	case MEM_CHUNK:
		return c->mem->len - c->offset;
	case FILE_CHUNK:
		return c->file.length - c->offset;
	}
	return 0;
}

INLINE chunkiter chunkqueue_iter(chunkqueue *cq) {
	chunkiter i;
	i.element = g_queue_peek_head_link(cq->queue);
	return i;
}

INLINE chunk* chunkqueue_first_chunk(chunkqueue *cq) {
	return (chunk*) g_queue_peek_head(cq->queue);
}

#endif

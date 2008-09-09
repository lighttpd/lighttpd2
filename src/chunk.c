
#include "base.h"
#include "chunk.h"
#include "log.h"

/******************
 *   chunkfile    *
 ******************/

static chunkfile *chunkfile_new(GString *name, int fd, gboolean is_temp) {
	chunkfile *cf = g_slice_new(chunkfile);
	cf->refcount = 1;
	if (name) {
		cf->name = g_string_new_len(GSTR_LEN(name));
	} else {
		cf->name = NULL;
	}
	cf->fd = fd;
	cf->is_temp = is_temp;
	return cf;
}

static void chunkfile_acquire(chunkfile *cf) {
	assert(g_atomic_int_get(&cf->refcount) > 0);
	g_atomic_int_inc(&cf->refcount);
}

static void chunkfile_release(chunkfile *cf) {
	if (!cf) return;
	assert(g_atomic_int_get(&cf->refcount) > 0);
	if (g_atomic_int_dec_and_test(&cf->refcount)) {
		if (-1 != cf->fd) close(cf->fd);
		cf->fd = -1;
		if (cf->is_temp) unlink(cf->name->str);
		cf->is_temp = FALSE;
		if (cf->name) g_string_free(cf->name, TRUE);
		cf->name = NULL;
		g_slice_free(chunkfile, cf);
	}
}

/* open the file cf->name if it is not already opened for reading
 * may return HANDLER_GO_ON, HANDLER_ERROR, HANDLER_WAIT_FOR_FD
 */
handler_t chunkfile_open(connection *con, chunkfile *cf) {
	if (!cf) return HANDLER_ERROR;
	if (-1 != cf->fd) return HANDLER_GO_ON;
	if (!cf->name) {
		CON_ERROR(con, "%s", "Missing filename for FILE_CHUNK");
		return HANDLER_ERROR;
	}
	if (-1 == (cf->fd = open(cf->name->str, O_RDONLY))) {
		if (EMFILE == errno) return HANDLER_WAIT_FOR_FD;
		CON_ERROR(con, "Couldn't open file '%s': %s", GSTR_SAFE_STR(cf->name), g_strerror(errno));
		return HANDLER_ERROR;
	}
#ifdef FD_CLOEXEC
	fcntl(cf->fd, F_SETFD, FD_CLOEXEC);
#endif
#ifdef HAVE_POSIX_FADVISE
	/* tell the kernel that we want to stream the file */
	if (-1 == posix_fadvise(cf->fd, 0, 0, POSIX_FADV_SEQUENTIAL)) {
		if (ENOSYS != errno) {
			CON_ERROR(con, "posix_fadvise failed for '%s': %s (%i)", GSTR_SAFE_STR(cf->name), g_strerror(errno), cf->fd);
		}
	}
#endif
	return HANDLER_GO_ON;
}

/******************
 * chunk iterator *
 ******************/

/* must be powers of 2 */
#define MAX_MMAP_CHUNK (2*1024*1024)
#define MMAP_CHUNK_ALIGN (4*1024)

/* get the data from a chunk; easy in case of a MEM_CHUNK,
 * but needs to do io in case of FILE_CHUNK; it tries mmap and
 * falls back to read(...)
 * the data is _not_ marked as "done"
 * may return HANDLER_GO_ON, HANDLER_ERROR, HANDLER_WAIT_FOR_FD
 */
handler_t chunkiter_read(connection *con, chunkiter iter, off_t start, off_t length, char **data_start, off_t *data_len) {
	chunk *c = chunkiter_chunk(iter);
	off_t we_want, we_have, our_start, our_offset;
	handler_t res = HANDLER_GO_ON;
	int mmap_errno = 0;

	if (!c) return HANDLER_ERROR;
	if (!data_start || !data_len) return HANDLER_ERROR;

	we_have = chunk_length(c) - start;
	if (length > we_have) length = we_have;
	if (length <= 0) return HANDLER_ERROR;

	switch (c->type) {
	case UNUSED_CHUNK: return HANDLER_ERROR;
	case MEM_CHUNK:
		*data_start = c->mem->str + c->offset + start;
		*data_len = length;
		break;
	case FILE_CHUNK:
		if (HANDLER_GO_ON != (res = chunkfile_open(con, c->file.file))) return res;

		if (length > MAX_MMAP_CHUNK) length = MAX_MMAP_CHUNK;

		if ( !(c->file.mmap.data != MAP_FAILED || c->mem) /* no data present */
			|| !( /* or in the wrong range */
				(start + c->offset + c->file.start >= c->file.mmap.offset)
				&& (start + c->offset + c->file.start + length <= c->file.mmap.offset + (ssize_t) c->file.mmap.length)) ) {
			/* then find new range */
			our_start = start + c->offset + c->file.start;  /* "start" maps to this offset in the file */
			our_offset = our_start % MMAP_CHUNK_ALIGN;      /* offset for "start" in new mmap block */
			our_start -= our_offset;                 /* file offset for mmap */

			we_want = length + MAX_MMAP_CHUNK;
			if (we_want > we_have) we_want = we_have;
			we_want += our_offset;

			if (MAP_FAILED != c->file.mmap.data) {
				munmap(c->file.mmap.data, c->file.mmap.length);
				c->file.mmap.data = MAP_FAILED;
			}
			c->file.mmap.offset = our_start;
			c->file.mmap.length = we_want;
			if (!c->mem) { /* mmap did not fail till now */
				c->file.mmap.data = mmap(0, we_want, PROT_READ, MAP_SHARED, c->file.file->fd, our_start);
				mmap_errno = errno;
			}
			if (MAP_FAILED == c->file.mmap.data) {
				/* fallback to read(...) */
				if (!c->mem) {
					c->mem = g_string_sized_new(we_want);
				} else {
					g_string_set_size(c->mem, we_want);
				}
				if (-1 == lseek(c->file.file->fd, our_start, SEEK_SET)) {
					/* prefer the error of the first syscall */
					if (0 != mmap_errno) {
						CON_ERROR(con, "mmap failed for '%s' (fd = %i): %s",
							GSTR_SAFE_STR(c->file.file->name), c->file.file->fd,
							g_strerror(mmap_errno));
					} else {
						CON_ERROR(con, "lseek failed for '%s' (fd = %i): %s",
							GSTR_SAFE_STR(c->file.file->name), c->file.file->fd,
							g_strerror(errno));
					}
					g_string_free(c->mem, TRUE);
					c->mem = NULL;
					return HANDLER_ERROR;
				}
read_chunk:
				if (-1 == (we_have = read(c->file.file->fd, c->mem->str, we_want))) {
					if (EINTR == errno) goto read_chunk;
					/* prefer the error of the first syscall */
					if (0 != mmap_errno) {
						CON_ERROR(con, "mmap failed for '%s' (fd = %i): %s",
							GSTR_SAFE_STR(c->file.file->name), c->file.file->fd,
							g_strerror(mmap_errno));
					} else {
						CON_ERROR(con, "read failed for '%s' (fd = %i): %s",
							GSTR_SAFE_STR(c->file.file->name), c->file.file->fd,
							g_strerror(errno));
					}
					g_string_free(c->mem, TRUE);
					c->mem = NULL;
					return HANDLER_ERROR;
				} else if (we_have != we_want) {
					/* may return less than requested bytes due to signals */
					/* CON_TRACE(srv, "read return unexpected number of bytes"); */
					we_want = we_have;
					if (length > we_have) length = we_have;
					c->file.mmap.length = we_want;
					g_string_set_size(c->mem, we_want);
				}
			} else {
#ifdef HAVE_MADVISE
				/* don't advise files < 64Kb */
				if (c->file.mmap.length > (64*1024) &&
					0 != madvise(c->file.mmap.data, c->file.mmap.length, MADV_WILLNEED)) {
					CON_ERROR(con, "madvise failed for '%s' (fd = %i): %s",
						GSTR_SAFE_STR(c->file.file->name), c->file.file->fd,
						g_strerror(errno));
				}
#endif
			}
		}
		*data_start = (c->mem ? c->mem->str : c->file.mmap.data) + start + c->offset + c->file.start - c->file.mmap.offset;
		*data_len = length;
		break;
	}
	return HANDLER_GO_ON;
}

/******************
 *     chunk      *
 ******************/

static chunk* chunk_new() {
	chunk *c = g_slice_new0(chunk);
	c->file.mmap.data = MAP_FAILED;
	return c;
}

/*
static void chunk_reset(chunk *c) {
	if (!c) return;
	c->type = UNUSED_CHUNK;
	c->offset = 0;
	if (c->mem) g_string_free(c->mem, TRUE);
	c->mem = NULL;
	if (c->file.file) chunkfile_release(c->file.file);
	c->file.file = NULL;
	c->file.start = 0;
	c->file.length = 0;
	if (MAP_FAILED != c->file.mmap.data) munmap(c->file.mmap.data, c->file.mmap.length);
	c->file.mmap.data = MAP_FAILED;
	c->file.mmap.length = 0;
	c->file.mmap.offset = 0;
}
*/

static void chunk_free(chunk *c) {
	if (!c) return;
	c->type = UNUSED_CHUNK;
	if (c->mem) g_string_free(c->mem, TRUE);
	c->mem = NULL;
	if (c->file.file) chunkfile_release(c->file.file);
	c->file.file = NULL;
	if (c->file.mmap.data != MAP_FAILED) munmap(c->file.mmap.data, c->file.mmap.length);
	c->file.mmap.data = MAP_FAILED;
	g_slice_free(chunk, c);
}

/******************
 *   chunkqueue   *
 ******************/

chunkqueue* chunkqueue_new() {
	chunkqueue *cq = g_slice_new0(chunkqueue);
	cq->queue = g_queue_new();
	return cq;
}

static void __chunk_free(gpointer c, gpointer UNUSED_PARAM(userdata)) {
	chunk_free((chunk*) c);
}

void chunkqueue_reset(chunkqueue *cq) {
	if (!cq) return;
	cq->is_closed = FALSE;
	cq->bytes_in = cq->bytes_out = cq->length = 0;
	g_queue_foreach(cq->queue, __chunk_free, NULL);
	g_queue_clear(cq->queue);
}

void chunkqueue_free(chunkqueue *cq) {
	if (!cq) return;
	g_queue_foreach(cq->queue, __chunk_free, NULL);
	g_queue_free(cq->queue);
	cq->queue = NULL;
	g_slice_free(chunkqueue, cq);
}

 /* pass ownership of str to chunkqueue, do not free/modify it afterwards
  * you may modify the data (not the length) if you are sure it isn't sent before.
  */
void chunkqueue_append_string(chunkqueue *cq, GString *str) {
	chunk *c;
	if (!str->len) return;
	c = chunk_new();
	c->type = MEM_CHUNK;
	c->mem = str;
	g_queue_push_tail(cq->queue, c);
	cq->length += str->len;
	cq->bytes_in += str->len;
}

/* memory gets copied */
void chunkqueue_append_mem(chunkqueue *cq, void *mem, gssize len) {
	chunk *c;
	if (!len) return;
	c = chunk_new();
	c->type = MEM_CHUNK;
	c->mem = g_string_new_len(mem, len);
	g_queue_push_tail(cq->queue, c);
	cq->length += c->mem->len;
	cq->bytes_in += c->mem->len;
}

static void __chunkqueue_append_file(chunkqueue *cq, GString *filename, off_t start, off_t length, int fd, gboolean is_temp) {
	chunk *c = chunk_new();
	c->type = FILE_CHUNK;
	c->file.file = chunkfile_new(filename, fd, is_temp);
	c->file.start = start;
	c->file.length = length;

	g_queue_push_tail(cq->queue, c);
	cq->length += length;
	cq->bytes_in += length;
}
/* pass ownership of filename, do not free it */
void chunkqueue_append_file(chunkqueue *cq, GString *filename, off_t start, off_t length) {
	if (length)
		__chunkqueue_append_file(cq, filename, start, length, -1, FALSE);
}

/* if you already opened the file, you can pass the fd here - do not close it */
void chunkqueue_append_file_fd(chunkqueue *cq, GString *filename, off_t start, off_t length, int fd) {
	if (length)
		__chunkqueue_append_file(cq, filename, start, length, fd, FALSE);
}

/* temp files get deleted after usage */
/* pass ownership of filename, do not free it */
void chunkqueue_append_tempfile(chunkqueue *cq, GString *filename, off_t start, off_t length) {
	if (length)
		__chunkqueue_append_file(cq, filename, start, length, -1, TRUE);
}

/* if you already opened the file, you can pass the fd here - do not close it */
void chunkqueue_append_tempfile_fd(chunkqueue *cq, GString *filename, off_t start, off_t length, int fd) {
	if (length)
		__chunkqueue_append_file(cq, filename, start, length, fd, TRUE);
}

/* steal up to length bytes from in and put them into out, return number of bytes stolen */
goffset chunkqueue_steal_len(chunkqueue *out, chunkqueue *in, goffset length) {
	chunk *c, *cnew;
	GList* l;
	goffset bytes = 0;
	goffset we_have;

	while ( (NULL != (c = chunkqueue_first_chunk(in))) && length > 0 ) {
		we_have = chunk_length(c);
		if (!we_have) { /* remove empty chunks */
			chunk_free(c);
			g_queue_pop_head(in->queue);
			continue;
		}
		if (we_have <= length) { /* move complete chunk */
			l = g_queue_pop_head_link(in->queue);
			g_queue_push_tail_link(out->queue, l);
			bytes += we_have;
			length -= we_have;
		} else { /* copy first part of a chunk */
			cnew = chunk_new();
			switch (c->type) {
			case UNUSED_CHUNK: /* impossible, has length 0 */
				/* remove "empty" chunks */
				chunk_free(c);
				chunk_free(cnew);
				g_queue_pop_head(in->queue);
				continue;
			case MEM_CHUNK:
				cnew->type = MEM_CHUNK;
				cnew->mem = g_string_new_len(c->mem->str + c->offset, length);
				break;
			case FILE_CHUNK:
				cnew->type = FILE_CHUNK;
				chunkfile_acquire(c->file.file);
				cnew->file.file = c->file.file;
				cnew->file.start = c->file.start + c->offset;
				cnew->file.length = length;
				break;
			}
			c->offset += length;
			bytes += length;
			length = 0;
			g_queue_push_tail(out->queue, cnew);
		}
	}

	in->bytes_out += bytes;
	in->length -= bytes;
	out->bytes_in += bytes;
	out->length += bytes;
	return bytes;
}

/* steal all chunks from in and put them into out, return number of bytes stolen */
goffset chunkqueue_steal_all(chunkqueue *out, chunkqueue *in) {
	/* if in->queue is empty, do nothing */
	if (!in->length) return 0;
	/* if out->queue is empty, just swap in->queue/out->queue */
	if (g_queue_is_empty(out->queue)) {
		GQueue *tmp = in->queue; in->queue = out->queue; out->queue = tmp;
	} else {
		/* link the two "lists", neither of them is empty */
		out->queue->tail->next = in->queue->head;
		in->queue->head->prev = out->queue->tail;
		/* update the queue tail and length */
		out->queue->tail = in->queue->tail;
		out->queue->length += in->queue->length;
		/* reset in->queue) */
		g_queue_init(in->queue);
	}
	/* count bytes in chunkqueues */
	goffset len = in->length;
	in->bytes_out += len;
	in->length = 0;
	out->bytes_in += len;
	out->length += len;
	return len;
}

/* steal the first chunk from in and append it to out, return number of bytes stolen */
goffset chunkqueue_steal_chunk(chunkqueue *out, chunkqueue *in) {
	goffset length;
	GList *l = g_queue_pop_head_link(in->queue);
	if (!l) return 0;
	g_queue_push_tail_link(out->queue, l);

	length = chunk_length((chunk*) l->data);
	in->bytes_out += length;
	in->length -= length;
	out->bytes_in += length;
	out->length += length;
	return length;
}

/* skip up to length bytes in a chunkqueue, return number of bytes skipped */
goffset chunkqueue_skip(chunkqueue *cq, goffset length) {
	chunk *c;
	goffset bytes = 0;
	goffset we_have;

	while ( (NULL != (c = chunkqueue_first_chunk(cq))) && (0 == (we_have = chunk_length(c)) || length > 0) ) {
		if (we_have <= length) {
			/* skip (delete) complete chunk */
			chunk_free(c);
			g_queue_pop_head(cq->queue);
			bytes += we_have;
			length -= we_have;
		} else { /* skip first part of a chunk */
			c->offset += length;
			bytes += length;
			length = 0;
		}
	}

	cq->bytes_out += bytes;
	cq->length -= bytes;
	return bytes;
}

goffset chunkqueue_skip_all(chunkqueue *cq) {
	goffset bytes = cq->length;

	g_queue_foreach(cq->queue, __chunk_free, NULL);
	g_queue_clear(cq->queue);

	cq->bytes_out += bytes;
	cq->length = 0;

	return bytes;
}

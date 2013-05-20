
#include "gnutls_filter.h"

struct liGnuTLSFilter {
	int refcount;
	const liGnuTLSFilterCallbacks *callbacks;
	gpointer callback_data;

	liServer *srv;
	liWorker *wrk;
	liLogContext *log_context; /* TODO: support setting this to VR context */

	gnutls_session_t session;
	liStream crypt_source;
	liStream crypt_drain;
	liStream plain_source;
	liStream plain_drain;

	liBuffer *raw_in_buffer; /* for gnutls_record_recv */
	liBuffer *raw_out_buffer; /* stream_pushv */

	unsigned int initial_handshaked_finished:1;
	unsigned int closing:1, aborted:1;
	unsigned int write_wants_read:1;
};

static ssize_t stream_push(gnutls_transport_ptr_t, const void*, size_t);
static ssize_t stream_pushv(gnutls_transport_ptr_t, const giovec_t * iov, int iovcnt);
static ssize_t stream_pull(gnutls_transport_ptr_t, void*, size_t);

static ssize_t stream_push(gnutls_transport_ptr_t trans, const void *buf, size_t len) {
	giovec_t vec;
	vec.iov_base = (void *) buf;
	vec.iov_len = len;
	return stream_pushv(trans, &vec, 1);
}
static ssize_t stream_pushv(gnutls_transport_ptr_t trans, const giovec_t * iov, int iovcnt) {
	const ssize_t blocksize = 16*1024; /* 16k */
	liGnuTLSFilter *f = (liGnuTLSFilter*) trans;
	liChunkQueue *cq;
	int i;
	liBuffer *buf;
	gboolean cq_buf_append;
	ssize_t written = 0;

	errno = ECONNRESET;

	if (NULL == f || NULL == f->crypt_source.out) return -1;
	cq = f->crypt_source.out;
	if (cq->is_closed) return -1;

	buf = f->raw_out_buffer;
	cq_buf_append = (buf != NULL && buf == li_chunkqueue_get_last_buffer(cq, 1024));
	for (i = 0; i < iovcnt; ++i) {
		const char *data = iov[i].iov_base;
		size_t len = iov[i].iov_len;

		while (len > 0) {
			size_t bufsize, do_write;
			if (NULL == buf) buf = li_buffer_new(blocksize);

			bufsize = buf->alloc_size - buf->used;
			do_write = (bufsize > len) ? len : bufsize;
			memcpy(buf->addr + buf->used, data, do_write);
			len -= do_write;
			data += do_write;
			if (cq_buf_append) {
				/* also updates buf->used */
				li_chunkqueue_update_last_buffer_size(cq, do_write);
			} else {
				gsize offset = buf->used;
				buf->used += do_write;
				li_buffer_acquire(buf);
				li_chunkqueue_append_buffer2(cq, buf, offset, do_write);
				cq_buf_append = TRUE;
			}
			if (buf->used == buf->alloc_size) {
				li_buffer_release(buf);
				buf = NULL;
				cq_buf_append = FALSE;
			}
			written += do_write;
		}
	}
	if (NULL != buf && buf->alloc_size - buf->used < 1024) {
		li_buffer_release(buf);
		f->raw_out_buffer = buf = NULL;
	} else {
		f->raw_out_buffer = buf;
	}

	li_stream_notify_later(&f->crypt_source);

	errno = 0;
	return written;
}
static ssize_t stream_pull(gnutls_transport_ptr_t trans, void *buf, size_t len) {
	liGnuTLSFilter *f = (liGnuTLSFilter*) trans;
	liChunkQueue *cq;

	errno = ECONNRESET;
	if (NULL == f || NULL == f->crypt_drain.out) return -1;

	cq = f->crypt_drain.out;

	if (0 == cq->length) {
		if (cq->is_closed) {
			errno = 0;
			return 0;
		} else {
			errno = EAGAIN;
			return -1;
		}
	}

	if (len > (size_t) cq->length) len = cq->length;
	if (!li_chunkqueue_extract_to_memory(cq, len, buf, NULL)) return -1;
	li_chunkqueue_skip(cq, len);

	errno = 0;
	return len;
}

static void f_close_gnutls(liGnuTLSFilter *f) {
	if (NULL != f->session && !f->closing) {
		liCQLimit *limit;
		f->closing = TRUE;
		f->session = NULL;

		assert(NULL != f->crypt_source.out);
		assert(NULL != f->crypt_source.out->limit);
		limit = f->crypt_source.out->limit;
		limit->notify = NULL;
		limit->context = NULL;

		li_stream_disconnect(&f->plain_source);

		li_stream_disconnect(&f->plain_drain);
		li_stream_disconnect_dest(&f->plain_source);

		f->log_context = NULL;
		if (NULL != f->callbacks && NULL != f->callbacks->closed_cb) {
			f->callbacks->closed_cb(f, f->callback_data);
		}
	}
}
static void f_acquire(liGnuTLSFilter *f) {
	assert(f->refcount > 0);
	++f->refcount;
}
static void f_release(liGnuTLSFilter *f) {
	assert(f->refcount > 0);
	if (0 == --f->refcount) {
		f->refcount = 1;
		f_close_gnutls(f);

		g_slice_free(liGnuTLSFilter, f);
	}
}
static void f_abort_gnutls(liGnuTLSFilter *f) {
	if (f->aborted) return;
	f->aborted = TRUE;
	f_acquire(f);
	f_close_gnutls(f);
	li_stream_disconnect(&f->crypt_drain);
	li_stream_disconnect_dest(&f->crypt_source);
	f_release(f);
}


static void do_handle_error(liGnuTLSFilter *f, const char *gnutlsfunc, int r, gboolean writing) {
	switch (r) {
	case GNUTLS_E_AGAIN:
		if (writing) f->write_wants_read = TRUE;
		break;
	case GNUTLS_E_REHANDSHAKE:
		if (f->initial_handshaked_finished) {
			_ERROR(f->srv, f->wrk, f->log_context, "%s: gnutls: client initiated renegotitation, closing connection", gnutlsfunc);
			f_abort_gnutls(f);
		}
		break;
	case GNUTLS_E_UNEXPECTED_PACKET_LENGTH:
		f_abort_gnutls(f);
		break;
	case GNUTLS_E_UNKNOWN_CIPHER_SUITE:
	case GNUTLS_E_UNSUPPORTED_VERSION_PACKET:
		_DEBUG(f->srv, f->wrk, f->log_context, "%s (%s): %s", gnutlsfunc,
			gnutls_strerror_name(r), gnutls_strerror(r));
		f_abort_gnutls(f);
		break;
	default:
		if (gnutls_error_is_fatal(r)) {
			_ERROR(f->srv, f->wrk, f->log_context, "%s (%s): %s", gnutlsfunc,
				gnutls_strerror_name(r), gnutls_strerror(r));
			f_abort_gnutls(f);
		} else {
			_ERROR(f->srv, f->wrk, f->log_context, "%s non fatal (%s): %s", gnutlsfunc,
				gnutls_strerror_name(r), gnutls_strerror(r));
		}
	}
}

#define TIMEDIFF_MS(ts1, ts2) (((ts2).tv_sec - (ts1).tv_sec)*1.0e3 + ((ts2).tv_nsec - (ts1).tv_nsec)*1.0e-6)

static gboolean do_gnutls_handshake(liGnuTLSFilter *f, gboolean writing) {
	int r;

	assert(!f->initial_handshaked_finished);

	r = gnutls_handshake(f->session);
	if (GNUTLS_E_SUCCESS == r) {
		f->initial_handshaked_finished = 1;
		li_stream_acquire(&f->plain_source);
		li_stream_acquire(&f->plain_drain);
		f->callbacks->handshake_cb(f, f->callback_data, &f->plain_source, &f->plain_drain);
		li_stream_release(&f->plain_source);
		li_stream_release(&f->plain_drain);
		return TRUE;
	} else {
		do_handle_error(f, "gnutls_handshake", r, writing);
		return FALSE;
	}
}

static void do_gnutls_read(liGnuTLSFilter *f) {
	const ssize_t blocksize = 16*1024; /* 16k */
	off_t max_read = 4 * blocksize; /* 64k */
	ssize_t r;
	off_t len = 0;
	liChunkQueue *cq = f->plain_source.out;

	f_acquire(f);

	if (NULL != f->session && !f->initial_handshaked_finished && !do_gnutls_handshake(f, FALSE)) goto out;
	if (NULL == f->session) {
		f_abort_gnutls(f);
		goto out;
	}

	do {
		liBuffer *buf;
		gboolean cq_buf_append;

		buf = li_chunkqueue_get_last_buffer(cq, 1024);
		cq_buf_append = (buf != NULL);

		if (buf != NULL) {
			/* use last buffer as raw_in_buffer; they should be the same anyway */
			if (G_UNLIKELY(buf != f->raw_in_buffer)) {
				li_buffer_acquire(buf);
				li_buffer_release(f->raw_in_buffer);
				f->raw_in_buffer = buf;
			}
		} else {
			buf = f->raw_in_buffer;
			if (buf != NULL && buf->alloc_size - buf->used < 1024) {
				/* release *buffer */
				li_buffer_release(buf);
				f->raw_in_buffer = buf = NULL;
			}
			if (buf == NULL) {
				f->raw_in_buffer = buf = li_buffer_new(blocksize);
			}
		}
		assert(f->raw_in_buffer == buf);

		r = gnutls_record_recv(f->session, buf->addr + buf->used, buf->alloc_size - buf->used);
		if (r < 0) {
			do_handle_error(f, "gnutls_record_recv", r, FALSE);
			goto out;
		} else if (r == 0) {
			/* clean shutdown? */
			f->plain_source.out->is_closed = TRUE;
			f->plain_drain.out->is_closed = TRUE;
			f->crypt_source.out->is_closed = TRUE;
			f->crypt_drain.out->is_closed = TRUE;
			li_stream_disconnect(&f->crypt_drain);
			li_stream_disconnect_dest(&f->crypt_source);
			f_close_gnutls(f);
			goto out;
		}

		if (cq_buf_append) {
			li_chunkqueue_update_last_buffer_size(cq, r);
		} else {
			gsize offset;

			li_buffer_acquire(buf);

			offset = buf->used;
			buf->used += r;
			li_chunkqueue_append_buffer2(cq, buf, offset, r);
		}
		if (buf->alloc_size - buf->used < 1024) {
			/* release *buffer */
			li_buffer_release(buf);
			f->raw_in_buffer = buf = NULL;
		}
		len += r;
	} while (len < max_read);

out:
	f_release(f);
}

#if GNUTLS_VERSION_NUMBER >= 0x030109
/* gnutls_record_cork / gnutls_record_uncork available since 3.1.9 */
#define USE_CORK
#endif

static void do_gnutls_write(liGnuTLSFilter *f) {
	const ssize_t blocksize = 16*1024; /* 16k */
	char *block_data;
	off_t block_len;
	ssize_t r;
	off_t write_max;
#ifdef USE_CORK
	gboolean corked = FALSE;
#endif
	liChunkQueue *cq = f->plain_drain.out;

	f_acquire(f);

	f->write_wants_read = FALSE;

	/* use space in (encrypted) outgoing buffer as amounts of bytes we try to write from (plain) output
	 * don't care if we write a little bit more than the limit allowed */
	write_max = li_chunkqueue_limit_available(f->crypt_source.out);
	assert(write_max >= 0); /* we set a limit! */
	if (0 == write_max) goto out;
	/* if we start writing, try to write at least blocksize bytes */
	if (write_max < blocksize) write_max = blocksize;

	if (NULL != f->session && !f->initial_handshaked_finished && !do_gnutls_handshake(f, TRUE)) goto out;
	if (NULL == f->session) {
		f_abort_gnutls(f);
		goto out;
	}

#ifdef USE_CORK
	if (0 != cq->length && cq->queue.length > 1) {
		corked = TRUE;
		gnutls_record_cork(f->session);
	}
#endif

	do {
		GError *err = NULL;
		liChunkIter ci;

		if (0 == cq->length) break;

		ci = li_chunkqueue_iter(cq);
		switch (li_chunkiter_read(ci, 0, blocksize, &block_data, &block_len, &err)) {
		case LI_HANDLER_GO_ON:
			break;
		case LI_HANDLER_ERROR:
			if (NULL != err) {
				_ERROR(f->srv, f->wrk, f->log_context, "Couldn't read data from chunkqueue: %s", err->message);
				g_error_free(err);
			}
			/* fall through */
		default:
			f_abort_gnutls(f);
			goto out;
		}

		r = gnutls_record_send(f->session, block_data, block_len);
		if (r <= 0) {
			do_handle_error(f, "gnutls_record_send", r, TRUE);
			goto out;
		}

		li_chunkqueue_skip(cq, r);
		write_max -= r;
	} while (r == block_len && write_max > 0);

	if (cq->is_closed && 0 == cq->length) {
		r = gnutls_bye(f->session, GNUTLS_SHUT_RDWR);
		switch (r) {
		case GNUTLS_E_SUCCESS:
		case GNUTLS_E_AGAIN:
		case GNUTLS_E_INTERRUPTED:
			f->plain_source.out->is_closed = TRUE;
			f->crypt_source.out->is_closed = TRUE;
			f->crypt_drain.out->is_closed = TRUE;
			f_close_gnutls(f);
			break;
		default:
			do_handle_error(f, "gnutls_bye", r, TRUE);
			f_abort_gnutls(f);
			break;
		}
	} else if (0 < cq->length && 0 != li_chunkqueue_limit_available(f->crypt_source.out)) {
		li_stream_again_later(&f->plain_drain);
	}

out:
#ifdef USE_CORK
	if (NULL != f->session && corked) {
		corked = TRUE;
		gnutls_record_uncork(f->session);
	}
#endif

	f_release(f);
}

/* ssl crypted out -> io */
static void stream_crypt_source_cb(liStream *stream, liStreamEvent event) {
	liGnuTLSFilter *f = LI_CONTAINER_OF(stream, liGnuTLSFilter, crypt_source);
	switch (event) {
	case LI_STREAM_NEW_DATA:
		/* data comes through SSL */
		break;
	case LI_STREAM_NEW_CQLIMIT:
		break;
	case LI_STREAM_CONNECTED_DEST: /* io out */
		break;
	case LI_STREAM_CONNECTED_SOURCE: /* plain_drain */
		break;
	case LI_STREAM_DISCONNECTED_DEST: /* io out disconnect */
		if (!stream->out->is_closed || 0 != stream->out->length) {
			f_abort_gnutls(f); /* didn't read everything */
		}
		break;
	case LI_STREAM_DISCONNECTED_SOURCE: /* plain_drain */
		if (!stream->out->is_closed) { /* f_close_ssl before we were ready */
			f_abort_gnutls(f);
		}
		break;
	case LI_STREAM_DESTROY:
		f_release(f);
		break;
	}
}
/* io -> ssl crypted in */
static void stream_crypt_drain_cb(liStream *stream, liStreamEvent event) {
	liGnuTLSFilter *f = LI_CONTAINER_OF(stream, liGnuTLSFilter, crypt_drain);
	switch (event) {
	case LI_STREAM_NEW_DATA:
		if (!stream->out->is_closed && NULL != stream->source) {
			li_chunkqueue_steal_all(stream->out, stream->source->out);
			stream->out->is_closed = stream->out->is_closed || stream->source->out->is_closed;
			li_stream_notify(stream); /* tell plain_source to do SSL_read */
		}
		if (stream->out->is_closed) {
			li_stream_disconnect(stream);
		}
		break;
	case LI_STREAM_NEW_CQLIMIT:
		break;
	case LI_STREAM_CONNECTED_DEST: /* plain_source */
		break;
	case LI_STREAM_CONNECTED_SOURCE: /* io in */
		break;
	case LI_STREAM_DISCONNECTED_DEST: /* plain_source */
		if (!stream->out->is_closed || 0 != stream->out->length) {
			f_abort_gnutls(f); /* didn't read everything */
		}
		break;
	case LI_STREAM_DISCONNECTED_SOURCE: /* io in disconnect */
		if (!stream->out->is_closed) {
			f_abort_gnutls(f); /* conn aborted */
		}
		break;
	case LI_STREAM_DESTROY:
		f_release(f);
		break;
	}
}
/* ssl (plain) -> app */
static void stream_plain_source_cb(liStream *stream, liStreamEvent event) {
	liGnuTLSFilter *f = LI_CONTAINER_OF(stream, liGnuTLSFilter, plain_source);
	switch (event) {
	case LI_STREAM_NEW_DATA:
		do_gnutls_read(f);
		if (f->write_wants_read) do_gnutls_write(f);
		li_stream_notify(stream);
		break;
	case LI_STREAM_NEW_CQLIMIT:
		break;
	case LI_STREAM_CONNECTED_DEST: /* app */
		break;
	case LI_STREAM_CONNECTED_SOURCE: /* crypt_drain */
		break;
	case LI_STREAM_DISCONNECTED_DEST: /* app */
		if (!stream->out->is_closed || 0 != stream->out->length) {
			f_abort_gnutls(f); /* didn't read everything */
		}
		break;
	case LI_STREAM_DISCONNECTED_SOURCE: /* crypt_drain */
		if (!stream->out->is_closed) {
			f_abort_gnutls(f); /* didn't get everything */
		}
		break;
	case LI_STREAM_DESTROY:
		f_release(f);
		break;
	}
}
/* app -> ssl (plain) */
static void stream_plain_drain_cb(liStream *stream, liStreamEvent event) {
	liGnuTLSFilter *f = LI_CONTAINER_OF(stream, liGnuTLSFilter, plain_drain);
	switch (event) {
	case LI_STREAM_NEW_DATA:
		if (!stream->out->is_closed && NULL != stream->source) {
			li_chunkqueue_steal_all(stream->out, stream->source->out);
			stream->out->is_closed = stream->out->is_closed || stream->source->out->is_closed;
		}
		do_gnutls_write(f);
		if (stream->out->is_closed) {
			li_stream_disconnect(stream);
			stream->out->is_closed = FALSE;
		}
		break;
	case LI_STREAM_NEW_CQLIMIT:
		break;
	case LI_STREAM_CONNECTED_DEST: /* crypt_source */
		break;
	case LI_STREAM_CONNECTED_SOURCE: /* app */
		break;
	case LI_STREAM_DISCONNECTED_DEST:
		if (!stream->out->is_closed || 0 != stream->out->length) {
			f_abort_gnutls(f); /* didn't read everything */
		}
		break;
	case LI_STREAM_DISCONNECTED_SOURCE:
		if (!stream->out->is_closed) {
			f_abort_gnutls(f); /* didn't get everything */
		}
		break;
	case LI_STREAM_DESTROY:
		f_release(f);
		break;
	}
}
static void stream_crypt_source_limit_notify_cb(gpointer context, gboolean locked) {
	liGnuTLSFilter *f = context;
	if (!locked) li_stream_again_later(&f->plain_drain);
}

static int post_client_hello_cb(gnutls_session_t session) {
	liGnuTLSFilter *f = gnutls_session_get_ptr(session);
	return f->callbacks->post_client_hello_cb(f, f->callback_data);
}

liGnuTLSFilter* li_gnutls_filter_new(
	liServer *srv, liWorker *wrk,
	const liGnuTLSFilterCallbacks *callbacks, gpointer data,
	gnutls_session_t session, liStream *crypt_source, liStream *crypt_drain
) {
	liEventLoop *loop = crypt_source->loop;
	liGnuTLSFilter *f;
	liCQLimit *out_limit;

	f = g_slice_new0(liGnuTLSFilter);
	f->refcount = 5; /* 1 + 4 streams */
	f->callbacks = callbacks;
	f->callback_data = data;
	f->srv = srv;
	f->wrk = wrk;

	f->session = session;
	gnutls_transport_set_ptr(f->session, (gnutls_transport_ptr_t) f);
	gnutls_transport_set_push_function(f->session, stream_push);
	gnutls_transport_set_vec_push_function(f->session, stream_pushv);
	gnutls_transport_set_pull_function(f->session, stream_pull);

	gnutls_session_set_ptr(f->session, f);
	gnutls_handshake_set_post_client_hello_function(f->session, post_client_hello_cb);

	f->initial_handshaked_finished = 0;
	f->closing = f->aborted = 0;
	f->write_wants_read = 0;

	li_stream_init(&f->crypt_source, loop, stream_crypt_source_cb);
	li_stream_init(&f->crypt_drain, loop, stream_crypt_drain_cb);
	li_stream_init(&f->plain_source, loop, stream_plain_source_cb);
	li_stream_init(&f->plain_drain, loop, stream_plain_drain_cb);

	/* "virtual" connections - the content goes through SSL */
	li_stream_connect(&f->plain_drain, &f->crypt_source);
	li_stream_connect(&f->crypt_drain, &f->plain_source);

	li_stream_connect(crypt_source, &f->crypt_drain);
	li_stream_connect(&f->crypt_source, crypt_drain);

	/* separate limit for buffer of encrypted data
	 *
	 * f->plain_drain is already connected to f->crypt_source,
	 *   so they won't share the same limit */
	out_limit = li_cqlimit_new();
	out_limit->notify = stream_crypt_source_limit_notify_cb;
	out_limit->context = f;
	li_cqlimit_set_limit(out_limit, 32*1024);
	li_chunkqueue_set_limit(crypt_drain->out, out_limit);
	li_chunkqueue_set_limit(f->crypt_source.out, out_limit);
	li_cqlimit_release(out_limit);

	return f;
}

void li_gnutls_filter_free(liGnuTLSFilter *f) {
	assert(NULL != f->callbacks);
	f->callbacks = NULL;
	f->callback_data = NULL;

	f_close_gnutls(f);

	li_stream_release(&f->crypt_source);
	li_stream_release(&f->crypt_drain);
	li_stream_release(&f->plain_source);
	li_stream_release(&f->plain_drain);
	f_release(f);
}


#include "openssl_filter.h"

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/rand.h>


struct liOpenSSLFilter {
	int refcount;
	const liOpenSSLFilterCallbacks *callbacks;
	gpointer callback_data;

	liServer *srv;
	liWorker *wrk;
	liLogContext *log_context; /* TODO: support setting this to VR context */

	SSL *ssl;
	BIO *bio;
	liStream crypt_source;
	liStream crypt_drain;
	liStream plain_source;
	liStream plain_drain;

	liBuffer *raw_in_buffer; /* for SSL_read */

	unsigned int initial_handshaked_finished:1;
	unsigned int client_initiated_renegotiation:1;
	unsigned int closing:1, aborted:1;
	unsigned int write_wants_read:1;
};

#define BIO_TYPE_LI_STREAM (127|BIO_TYPE_SOURCE_SINK)

static int stream_bio_write(BIO *bio, const char *buf, int len);
static int stream_bio_read(BIO *bio, char *buf, int len);
static int stream_bio_puts(BIO *bio, const char *str);
static int stream_bio_gets(BIO *bio, char *str, int len);
static long stream_bio_ctrl(BIO *bio, int cmd, long num, void *ptr);
static int stream_bio_create(BIO *bio);
static int stream_bio_destroy(BIO *bio);

static BIO_METHOD chunkqueue_bio_method = {
	BIO_TYPE_LI_STREAM,
	"lighttpd stream glue",
	stream_bio_write,
	stream_bio_read,
	stream_bio_puts,
	stream_bio_gets,
	stream_bio_ctrl,
	stream_bio_create,
	stream_bio_destroy,
	NULL
};

static int stream_bio_write(BIO *bio, const char *buf, int len) {
	liOpenSSLFilter *f = bio->ptr;
	liChunkQueue *cq;

	errno = ECONNRESET;

	if (NULL == f || NULL == f->crypt_source.out) return -1;
	cq = f->crypt_source.out;
	if (cq->is_closed) return -1;

	li_chunkqueue_append_mem(cq, buf, len);
	li_stream_notify_later(&f->crypt_source);

	errno = 0;
	return len;
}
static int stream_bio_read(BIO *bio, char *buf, int len) {
	liOpenSSLFilter *f = bio->ptr;
	liChunkQueue *cq;

	errno = ECONNRESET;
	BIO_clear_retry_flags(bio);

	if (NULL == f || NULL == f->crypt_drain.out) return -1;

	cq = f->crypt_drain.out;

	if (0 == cq->length) {
		if (cq->is_closed) {
			errno = 0;
			return 0;
		} else {
			errno = EAGAIN;
			BIO_set_retry_read(bio);
			return -1;
		}
	}

	if (len > cq->length) len = cq->length;
	if (!li_chunkqueue_extract_to_memory(cq, len, buf, NULL)) return -1;
	li_chunkqueue_skip(cq, len);

	errno = 0;
	return len;
}
static int stream_bio_puts(BIO *bio, const char *str) {
	return stream_bio_write(bio, str, strlen(str));
}
static int stream_bio_gets(BIO *bio, char *str, int len) {
	UNUSED(bio); UNUSED(str); UNUSED(len);
	return -1;
}
static long stream_bio_ctrl(BIO *bio, int cmd, long num, void *ptr) {
	liOpenSSLFilter *f = bio->ptr;
	UNUSED(num); UNUSED(ptr);

	switch (cmd) {
	case BIO_CTRL_FLUSH:
		return 1;
	case BIO_CTRL_PENDING:
		if (NULL == f || NULL == f->crypt_drain.out) return 0;
		return f->crypt_drain.out->length;
	default:
		return 0;
	}
}
static int stream_bio_create(BIO *bio) {
	bio->ptr = NULL;
	bio->init = 1;
	bio->shutdown = 1;
	bio->num = 0;
	bio->flags = 0;
	return 1;
}
static int stream_bio_destroy(BIO *bio) {
	liOpenSSLFilter *f = bio->ptr;
	bio->ptr = NULL;
	if (NULL != f) f->bio = NULL;
	bio->init = 0;
	return 1;
}

static void f_close_ssl(liOpenSSLFilter *f) {
	if (NULL != f->ssl && !f->closing) {
		SSL *ssl;
		liCQLimit *limit;

		f->closing = TRUE;

		assert(NULL != f->crypt_source.out);
		assert(NULL != f->crypt_source.out->limit);
		limit = f->crypt_source.out->limit;
		limit->notify = NULL;
		limit->context = NULL;

		li_stream_disconnect(&f->plain_source); /* crypt in -> plain out */

		li_stream_disconnect(&f->plain_drain); /* app -> plain in */
		li_stream_disconnect_dest(&f->plain_source); /* plain out -> app */

		f->log_context = NULL;
		if (NULL != f->callbacks && NULL != f->callbacks->closed_cb) {
			f->callbacks->closed_cb(f, f->callback_data);
		}
		ssl = f->ssl;
		f->ssl = NULL;
		if (NULL != ssl) SSL_free(ssl);
	}
}
static void f_acquire(liOpenSSLFilter *f) {
	assert(f->refcount > 0);
	++f->refcount;
}
static void f_release(liOpenSSLFilter *f) {
	assert(f->refcount > 0);
	if (0 == --f->refcount) {
		f->refcount = 1;
		f_close_ssl(f);
		if (NULL != f->bio) {
			BIO_free(f->bio);
			f->bio = NULL;
		}
		if (NULL != f->raw_in_buffer) {
			li_buffer_release(f->raw_in_buffer);
			f->raw_in_buffer = NULL;
		}

		g_slice_free(liOpenSSLFilter, f);
	}
}
static void f_abort_ssl(liOpenSSLFilter *f) {
	if (f->aborted) return;
	f->aborted = TRUE;
	f_acquire(f);
	f_close_ssl(f);
	li_stream_disconnect(&f->crypt_source); /* plain in -> crypt out */
	li_stream_disconnect(&f->crypt_drain); /* io -> crypt in */
	li_stream_disconnect_dest(&f->crypt_source); /* crypt out -> io */
	f_release(f);
}

static void do_handle_error(liOpenSSLFilter *f, const char *sslfunc, int r, gboolean writing) {
	int oerrno = errno, err;
	gboolean was_fatal;

	err = SSL_get_error(f->ssl, r);

	switch (err) {
	case SSL_ERROR_WANT_READ:
		if (writing) f->write_wants_read = TRUE;
		break;
	/*
	case SSL_ERROR_WANT_WRITE:
		we buffer all writes, can't happen! - handle as fatal error below
		break;
	*/
	case SSL_ERROR_SYSCALL:
		/**
			* man SSL_get_error()
			*
			* SSL_ERROR_SYSCALL
			*   Some I/O error occurred.  The OpenSSL error queue may contain more
			*   information on the error.  If the error queue is empty (i.e.
			*   ERR_get_error() returns 0), ret can be used to find out more about
			*   the error: If ret == 0, an EOF was observed that violates the
			*   protocol.  If ret == -1, the underlying BIO reported an I/O error
			*   (for socket I/O on Unix systems, consult errno for details).
			*
			*/
		while (0 != (err = ERR_get_error())) {
			_ERROR(f->srv, f->wrk, f->log_context, "%s: %s", sslfunc,
				ERR_error_string(err, NULL));
		}

		if (0 != r || (0 != oerrno && ECONNRESET != oerrno)) {
			_ERROR(f->srv, f->wrk, f->log_context, "%s returned %i: %s", sslfunc,
				r,
				g_strerror(oerrno));
		}
		f_abort_ssl(f);

		break;
	case SSL_ERROR_ZERO_RETURN:
		/* clean shutdown on the remote side */
		f->plain_source.out->is_closed = TRUE;
		li_stream_notify(&f->plain_source);
		li_stream_disconnect(&f->crypt_drain);
		li_stream_disconnect_dest(&f->crypt_source);
		break;
	default:
		was_fatal = FALSE;

		while((err = ERR_get_error())) {
			switch (ERR_GET_REASON(err)) {
			case SSL_R_SSL_HANDSHAKE_FAILURE:
			case SSL_R_TLSV1_ALERT_UNKNOWN_CA:
			case SSL_R_SSLV3_ALERT_CERTIFICATE_UNKNOWN:
			case SSL_R_SSLV3_ALERT_BAD_CERTIFICATE:
			case SSL_R_NO_SHARED_CIPHER:
			case SSL_R_UNKNOWN_PROTOCOL:
				/* TODO: if (!con->conf.log_ssl_noise) */ continue;
				break;
			default:
				was_fatal = TRUE;
				break;
			}
			/* get all errors from the error-queue */
			_ERROR(f->srv, f->wrk, f->log_context, "%s: %s", sslfunc,
				ERR_error_string(err, NULL));
		}
		if (!was_fatal) f_abort_ssl(f);
	}

}

static gboolean do_ssl_handshake(liOpenSSLFilter *f, gboolean writing) {
	int r = SSL_do_handshake(f->ssl);
	if (1 == r) {
		f->initial_handshaked_finished = 1;
		f->ssl->s3->flags |= SSL3_FLAGS_NO_RENEGOTIATE_CIPHERS;
		li_stream_acquire(&f->plain_source);
		li_stream_acquire(&f->plain_drain);
		f->callbacks->handshake_cb(f, f->callback_data, &f->plain_source, &f->plain_drain);
		li_stream_release(&f->plain_source);
		li_stream_release(&f->plain_drain);
		return TRUE;
	} else {
		do_handle_error(f, "SSL_do_handshake", r, writing);
		return FALSE;
	}
}

static void do_ssl_read(liOpenSSLFilter *f) {
	const ssize_t blocksize = 16*1024; /* 16k */
	off_t max_read = 4 * blocksize; /* 64k */
	ssize_t r;
	off_t len = 0;
	liChunkQueue *cq = f->plain_source.out;

	f_acquire(f);

	if (NULL != f->ssl && !f->initial_handshaked_finished && !do_ssl_handshake(f, FALSE)) goto out;
	if (NULL == f->ssl) {
		f_abort_ssl(f);
		goto out;
	}

	do {
		liBuffer *buf;
		gboolean cq_buf_append;

		ERR_clear_error();

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

		r = SSL_read(f->ssl, buf->addr + buf->used, buf->alloc_size - buf->used);
		if (f->client_initiated_renegotiation) {
			_ERROR(f->srv, f->wrk, f->log_context, "%s", "SSL: client initiated renegotitation, closing connection");
			f_abort_ssl(f);
			goto out;
		}
		if (r < 0) {
			do_handle_error(f, "SSL_read", r, FALSE);
			goto out;
		} else if (r == 0) {
			/* clean shutdown? */
			r = SSL_shutdown(f->ssl);
			switch (r) {
			case 0: /* don't care about bidirectional shutdown */
			case 1: /* bidirectional shutdown finished */
				f->plain_source.out->is_closed = TRUE;
				f->plain_drain.out->is_closed = TRUE;
				f->crypt_source.out->is_closed = TRUE;
				f->crypt_drain.out->is_closed = TRUE;
				li_stream_disconnect(&f->crypt_drain); /* io -> crypt in */
				li_stream_disconnect_dest(&f->crypt_source); /* crypt out -> io */
				li_stream_disconnect(&f->crypt_source); /* plain in -> crypt out */
				f_close_ssl(f);
				break;
			default:
				do_handle_error(f, "SSL_shutdown", r, TRUE);
				f_abort_ssl(f);
				break;
			}
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

static void do_ssl_write(liOpenSSLFilter *f) {
	const ssize_t blocksize = 16*1024; /* 16k */
	char *block_data;
	off_t block_len;
	ssize_t r;
	off_t write_max = 64*1024;
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

	if (NULL != f->ssl && !f->initial_handshaked_finished && !do_ssl_handshake(f, TRUE)) goto out;
	if (NULL == f->ssl) {
		f_abort_ssl(f);
		goto out;
	}

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
			f_abort_ssl(f);
			goto out;
		}

		/**
		 * SSL_write man-page
		 *
		 * WARNING
		 *        When an SSL_write() operation has to be repeated because of
		 *        SSL_ERROR_WANT_READ or SSL_ERROR_WANT_WRITE, it must be
		 *        repeated with the same arguments.
		 *
		 */

		ERR_clear_error();
		r = SSL_write(f->ssl, block_data, block_len);
		if (f->client_initiated_renegotiation) {
			_ERROR(f->srv, f->wrk, f->log_context, "%s", "SSL: client initiated renegotitation, closing connection");
			f_abort_ssl(f);
			goto out;
		}
		if (r <= 0) {
			do_handle_error(f, "SSL_write", r, TRUE);
			goto out;
		}

		li_chunkqueue_skip(cq, r);
		write_max -= r;
	} while (r == block_len && write_max > 0);

	if (cq->is_closed && 0 == cq->length) {
		r = SSL_shutdown(f->ssl);
		switch (r) {
		case 0: /* don't care about bidirectional shutdown */
		case 1: /* bidirectional shutdown finished */
			f->plain_source.out->is_closed = TRUE;
			f->crypt_source.out->is_closed = TRUE;
			f->crypt_drain.out->is_closed = TRUE;
			li_stream_disconnect(&f->crypt_source); /* plain in -> crypt out */
			f_close_ssl(f);
			break;
		default:
			do_handle_error(f, "SSL_shutdown", r, TRUE);
			f_abort_ssl(f);
			break;
		}
	} else if (0 < cq->length && 0 != li_chunkqueue_limit_available(f->crypt_source.out)) {
		li_stream_again_later(&f->plain_drain);
	}

out:
	f_release(f);
}

/* ssl crypted out -> io */
static void stream_crypt_source_cb(liStream *stream, liStreamEvent event) {
	liOpenSSLFilter *f = LI_CONTAINER_OF(stream, liOpenSSLFilter, crypt_source);
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
			f_abort_ssl(f); /* didn't read everything */
		}
		break;
	case LI_STREAM_DISCONNECTED_SOURCE: /* plain_drain */
		if (!stream->out->is_closed) { /* f_close_ssl before we were ready */
			f_abort_ssl(f);
		}
		break;
	case LI_STREAM_DESTROY:
		f_release(f);
		break;
	}
}
/* io -> ssl crypted in */
static void stream_crypt_drain_cb(liStream *stream, liStreamEvent event) {
	liOpenSSLFilter *f = LI_CONTAINER_OF(stream, liOpenSSLFilter, crypt_drain);
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
			f_abort_ssl(f); /* didn't read everything */
		}
		break;
	case LI_STREAM_DISCONNECTED_SOURCE: /* io in disconnect */
		if (!stream->out->is_closed) {
			f_abort_ssl(f); /* conn aborted */
		}
		break;
	case LI_STREAM_DESTROY:
		f_release(f);
		break;
	}
}
/* ssl (plain) -> app */
static void stream_plain_source_cb(liStream *stream, liStreamEvent event) {
	liOpenSSLFilter *f = LI_CONTAINER_OF(stream, liOpenSSLFilter, plain_source);
	switch (event) {
	case LI_STREAM_NEW_DATA:
		do_ssl_read(f);
		if (f->write_wants_read) do_ssl_write(f);
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
			f_abort_ssl(f); /* didn't read everything */
		}
		break;
	case LI_STREAM_DISCONNECTED_SOURCE: /* crypt_drain */
		if (!stream->out->is_closed) {
			f_abort_ssl(f); /* didn't get everything */
		}
		break;
	case LI_STREAM_DESTROY:
		f_release(f);
		break;
	}
}
/* app -> ssl (plain) */
static void stream_plain_drain_cb(liStream *stream, liStreamEvent event) {
	liOpenSSLFilter *f = LI_CONTAINER_OF(stream, liOpenSSLFilter, plain_drain);
	switch (event) {
	case LI_STREAM_NEW_DATA:
		if (!stream->out->is_closed && NULL != stream->source) {
			li_chunkqueue_steal_all(stream->out, stream->source->out);
			stream->out->is_closed = stream->out->is_closed || stream->source->out->is_closed;
		}
		do_ssl_write(f);
		if (stream->out->is_closed) {
			li_stream_disconnect(stream);
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
			f_abort_ssl(f); /* didn't read everything */
		}
		break;
	case LI_STREAM_DISCONNECTED_SOURCE:
		if (!stream->out->is_closed) {
			f_abort_ssl(f); /* didn't get everything */
		}
		break;
	case LI_STREAM_DESTROY:
		f_release(f);
		break;
	}
}
static void stream_crypt_source_limit_notify_cb(gpointer context, gboolean locked) {
	liOpenSSLFilter *f = context;
	if (!locked && !f->closing) li_stream_again_later(&f->plain_drain);
}

static void openssl_info_callback(const SSL *ssl, int where, int ret) {
	UNUSED(ret);

	if (0 != (where & SSL_CB_HANDSHAKE_START)) {
		liOpenSSLFilter *f = SSL_get_app_data(ssl);
		if (f->initial_handshaked_finished) {
			f->client_initiated_renegotiation = TRUE;
		}
	}
}

liOpenSSLFilter* li_openssl_filter_new(
	liServer *srv, liWorker *wrk,
	const liOpenSSLFilterCallbacks *callbacks, gpointer data,
	SSL_CTX *ssl_ctx, liStream *crypt_source, liStream *crypt_drain
) {
	liEventLoop *loop = crypt_source->loop;
	liOpenSSLFilter *f;
	SSL *ssl;
	liCQLimit *out_limit;

	ssl = SSL_new(ssl_ctx);
	if (NULL == ssl) return NULL;

	f = g_slice_new0(liOpenSSLFilter);
	f->refcount = 5; /* 1 + 4 streams */
	f->callbacks = callbacks;
	f->callback_data = data;
	f->srv = srv;
	f->wrk = wrk;

	f->ssl = ssl;
	SSL_set_app_data(f->ssl, f);
	SSL_set_info_callback(f->ssl, openssl_info_callback);
	f->bio = BIO_new(&chunkqueue_bio_method);
	f->bio->ptr = f;
	SSL_set_bio(f->ssl, f->bio, f->bio);
	/* BIO_set_callback(f->bio, BIO_debug_callback); */

	f->initial_handshaked_finished = 0;
	f->client_initiated_renegotiation = 0;
	f->closing = f->aborted = 0;
	f->write_wants_read = 0;

	li_stream_init(&f->crypt_source, loop, stream_crypt_source_cb);
	li_stream_init(&f->crypt_drain, loop, stream_crypt_drain_cb);
	li_stream_init(&f->plain_source, loop, stream_plain_source_cb);
	li_stream_init(&f->plain_drain, loop, stream_plain_drain_cb);

	/* "virtual" connections - the content goes through SSL. needed for liCQLimit. */
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

	SSL_set_accept_state(f->ssl);

	return f;
}

void li_openssl_filter_free(liOpenSSLFilter *f) {
	assert(NULL != f->callbacks);
	f->callbacks = NULL;
	f->callback_data = NULL;

	f_close_ssl(f);

	li_stream_release(&f->crypt_source);
	li_stream_release(&f->crypt_drain);
	li_stream_release(&f->plain_source);
	li_stream_release(&f->plain_drain);
	f_release(f);
}

SSL* li_openssl_filter_ssl(liOpenSSLFilter *f) {
	return f->ssl;
}

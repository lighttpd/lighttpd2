
#include <lighttpd/base.h>

#include <sys/uio.h>

#ifndef UIO_MAXIOV
# if defined(__FreeBSD__) || defined(__APPLE__) || defined(__NetBSD__)
/* FreeBSD 4.7 defines it in sys/uio.h only if _KERNEL is specified */
#  define UIO_MAXIOV 1024
# elif defined(__sgi)
/* IRIX 6.5 has sysconf(_SC_IOV_MAX) which might return 512 or bigger */
#  define UIO_MAXIOV 512
# elif defined(__sun)
/* Solaris (and SunOS?) defines IOV_MAX instead */
#  ifndef IOV_MAX
#   define UIO_MAXIOV 16
#  else
#   define UIO_MAXIOV IOV_MAX
#  endif
# elif defined(IOV_MAX)
#  define UIO_MAXIOV IOV_MAX
# else
#  error UIO_MAXIOV nor IOV_MAX are defined
# endif
#endif

/* first chunk must be a STRING_CHUNK ! */
liNetworkStatus li_network_backend_writev(int fd, liChunkQueue *cq, goffset *write_max, GError **err) {
	off_t we_have;
	ssize_t r;
	gboolean did_write_something = FALSE;
	liChunkIter ci;
	liChunk *c;
	liNetworkStatus res = LI_NETWORK_STATUS_FATAL_ERROR;

	GArray *chunks = g_array_sized_new(FALSE, TRUE, sizeof(struct iovec), UIO_MAXIOV);

	if (0 == cq->length) goto cleanup; /* FATAL ERROR */

	do {
		ci = li_chunkqueue_iter(cq);

		if (STRING_CHUNK != (c = li_chunkiter_chunk(ci))->type && MEM_CHUNK != c->type && BUFFER_CHUNK != c->type) {
			res = did_write_something ? LI_NETWORK_STATUS_SUCCESS : LI_NETWORK_STATUS_FATAL_ERROR;
			goto cleanup;
		}

		we_have = 0;
		do {
			guint i = chunks->len;
			off_t len = li_chunk_length(c);
			struct iovec *v;
			g_array_set_size(chunks, i + 1);
			v = &g_array_index(chunks, struct iovec, i);
			if (c->type == STRING_CHUNK) {
				v->iov_base = c->data.str->str + c->offset;
			} else if (c->type == MEM_CHUNK) {
				v->iov_base = c->mem->data + c->offset;
			} else { /* if (c->type == BUFFER_CHUNK) */
				v->iov_base = c->data.buffer.buffer->addr + c->data.buffer.offset + c->offset;
			}
			if (len > *write_max - we_have) len = *write_max - we_have;
			v->iov_len = len;
			we_have += len;
		} while (we_have < *write_max &&
		         li_chunkiter_next(&ci) &&
		         (STRING_CHUNK == (c = li_chunkiter_chunk(ci))->type || MEM_CHUNK == c->type || BUFFER_CHUNK == c->type) &&
		         chunks->len < UIO_MAXIOV);

		while (-1 == (r = writev(fd, (struct iovec*) chunks->data, chunks->len))) {
			switch (errno) {
			case EAGAIN:
#if EWOULDBLOCK != EAGAIN
			case EWOULDBLOCK:
#endif
				res = LI_NETWORK_STATUS_WAIT_FOR_EVENT;
				goto cleanup;
			case ECONNRESET:
			case EPIPE:
			case ETIMEDOUT:
				res = LI_NETWORK_STATUS_CONNECTION_CLOSE;
				goto cleanup;
			case EINTR:
				break; /* try again */
			default:
				g_set_error(err, LI_NETWORK_ERROR, 0, "li_network_backend_writev: oops, write to fd=%d failed: %s", fd, g_strerror(errno));
				goto cleanup;
			}
		}
		if (0 == r) {
			res = LI_NETWORK_STATUS_WAIT_FOR_EVENT;
			goto cleanup;
		}
		li_chunkqueue_skip(cq, r);
		*write_max -= r;

		if (r != we_have) {
			res = LI_NETWORK_STATUS_WAIT_FOR_EVENT;
			goto cleanup;
		}

		if (0 == cq->length) {
			res = LI_NETWORK_STATUS_SUCCESS;
			goto cleanup;
		}

		did_write_something = TRUE;
		g_array_set_size(chunks, 0);
	} while (*write_max > 0);

	res = LI_NETWORK_STATUS_SUCCESS;

cleanup:
	g_array_free(chunks, TRUE);
	return res;
}

liNetworkStatus li_network_write_writev(int fd, liChunkQueue *cq, goffset *write_max, GError **err) {
	if (cq->length == 0) return LI_NETWORK_STATUS_FATAL_ERROR;
	do {
		switch (li_chunkqueue_first_chunk(cq)->type) {
		case STRING_CHUNK:
		case MEM_CHUNK:
		case BUFFER_CHUNK:
			LI_NETWORK_FALLBACK(li_network_backend_writev, write_max);
			break;
		case FILE_CHUNK:
			LI_NETWORK_FALLBACK(li_network_backend_write, write_max);
			break;
		default:
			return LI_NETWORK_STATUS_FATAL_ERROR;
		}
		if (cq->length == 0) return LI_NETWORK_STATUS_SUCCESS;
	} while (*write_max > 0);
	return LI_NETWORK_STATUS_SUCCESS;
}

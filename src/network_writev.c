
#include "network.h"

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

/* first chunk must be a MEM_CHUNK ! */
network_status_t network_backend_writev(server *srv, connection *con, int fd, chunkqueue *cq, goffset *write_max) {
	off_t we_have;
	ssize_t r;
	gboolean did_write_something = FALSE;
	chunkiter ci;
	chunk *c;
	network_status_t res = NETWORK_STATUS_FATAL_ERROR;

	GArray *chunks = g_array_sized_new(FALSE, TRUE, sizeof(struct iovec), UIO_MAXIOV);

	if (0 == cq->length) goto cleanup; /* FATAL ERROR */

	do {
		ci = chunkqueue_iter(cq);

		if (MEM_CHUNK != (c = chunkiter_chunk(ci))->type) {
			res = did_write_something ? NETWORK_STATUS_SUCCESS : NETWORK_STATUS_FATAL_ERROR;
			goto cleanup;
		}

		we_have = 0;
		do {
			guint i = chunks->len;
			off_t len = c->mem->len - c->offset;
			struct iovec *v;
			g_array_set_size(chunks, i + 1);
			v = &g_array_index(chunks, struct iovec, i);
			v->iov_base = c->mem->str + c->offset;
			if (len > *write_max - we_have) len = *write_max - we_have;
			v->iov_len = len;
			we_have += len;
		} while (we_have < *write_max &&
		         chunkiter_next(&ci) &&
		         MEM_CHUNK == (c = chunkiter_chunk(ci))->type &&
		         chunks->len < UIO_MAXIOV);

		while (-1 == (r = writev(fd, (struct iovec*) chunks->data, chunks->len))) {
			switch (errno) {
			case EAGAIN:
#if EWOULDBLOCK != EAGAIN
			case EWOULDBLOCK
#endif
				res = did_write_something ? NETWORK_STATUS_SUCCESS : NETWORK_STATUS_WAIT_FOR_EVENT;
				goto cleanup;
			case ECONNRESET:
			case EPIPE:
				res = NETWORK_STATUS_CONNECTION_CLOSE;
				goto cleanup;
			case EINTR:
				break; /* try again */
			default:
				CON_ERROR(srv, con, "oops, write to fd=%d failed: %s", fd, g_strerror(errno));
				goto cleanup;
			}
		}
		if (0 == r) {
			res = did_write_something ? NETWORK_STATUS_SUCCESS : NETWORK_STATUS_WAIT_FOR_EVENT;
			goto cleanup;
		}
		chunkqueue_skip(cq, r);
		*write_max -= r;
		if (r != we_have) {
			res = NETWORK_STATUS_SUCCESS;
			goto cleanup;
		}

		if (0 == cq->length) {
			res = NETWORK_STATUS_SUCCESS;
			goto cleanup;
		}


		did_write_something = TRUE;
		g_array_set_size(chunks, 0);
	} while (*write_max > 0);

	res = NETWORK_STATUS_SUCCESS;

cleanup:
	g_array_free(chunks, TRUE);
	return res;
}

network_status_t network_write_writev(server *srv, connection *con, int fd, chunkqueue *cq) {
	goffset write_max = 256*1024; // 256k //;
	if (cq->length == 0) return NETWORK_STATUS_FATAL_ERROR;
	do {
		switch (chunkqueue_first_chunk(cq)->type) {
		case MEM_CHUNK:
			NETWORK_FALLBACK(network_backend_writev, &write_max);
			break;
		case FILE_CHUNK:
			NETWORK_FALLBACK(network_backend_write, &write_max);
			break;
		default:
			return NETWORK_STATUS_FATAL_ERROR;
		}
		if (cq->length == 0) return NETWORK_STATUS_SUCCESS;
	} while (write_max > 0);
	return NETWORK_STATUS_SUCCESS;
}

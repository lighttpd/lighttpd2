
#include <lighttpd/base.h>
#include <lighttpd/plugin_core.h>

/** repeats write after EINTR */
ssize_t net_write(int fd, void *buf, ssize_t nbyte) {
	ssize_t r;
	while (-1 == (r = write(fd, buf, nbyte))) {
		switch (errno) {
		case EINTR:
			/* Try again */
			break;
		default:
			/* report error */
			return r;
		}
	}
	/* return bytes written */
	return r;
}

/** repeats read after EINTR */
ssize_t net_read(int fd, void *buf, ssize_t nbyte) {
	ssize_t r;
	while (-1 == (r = read(fd, buf, nbyte))) {
		switch (errno) {
		case EINTR:
			/* Try again */
			break;
		default:
			/* report error */
			return r;
		}
	}
	/* return bytes read */
	return r;
}

network_status_t network_write(vrequest *vr, int fd, chunkqueue *cq, goffset write_max) {
	network_status_t res;
#ifdef TCP_CORK
	int corked = 0;
#endif
	goffset write_bytes, wrote;

#ifdef TCP_CORK
	/* Linux: put a cork into the socket as we want to combine the write() calls
	 * but only if we really have multiple chunks
	 */
	if (cq->queue->length > 1) {
		corked = 1;
		setsockopt(fd, IPPROTO_TCP, TCP_CORK, &corked, sizeof(corked));
	}
#endif

	write_bytes = write_max;
	/* TODO: add setup-option to select the backend */
#ifdef USE_SENDFILE
	res = network_write_sendfile(vr, fd, cq, &write_bytes);
#else
	res = network_write_writev(vr, fd, cq, &write_bytes);
#endif
	wrote = write_max - write_bytes;
	if (wrote > 0 && res == NETWORK_STATUS_WAIT_FOR_EVENT) res = NETWORK_STATUS_SUCCESS;

#ifdef TCP_CORK
	if (corked) {
		corked = 0;
		setsockopt(fd, IPPROTO_TCP, TCP_CORK, &corked, sizeof(corked));
	}
#endif

	return res;
}

network_status_t network_read(vrequest *vr, int fd, chunkqueue *cq) {
	const ssize_t blocksize = 16*1024; /* 16k */
	off_t max_read = 16 * blocksize; /* 256k */
	ssize_t r;
	off_t len = 0;

	if (cq->limit && cq->limit->limit > 0) {
		if (max_read > cq->limit->limit - cq->limit->current) {
			max_read = cq->limit->limit - cq->limit->current;
			if (max_read <= 0) {
				max_read = 0; /* we still have to read something */
				VR_ERROR(vr, "%s", "network_read: fd should be disabled as chunkqueue is already full");
			}
		}
	}

	do {
		GByteArray *buf = g_byte_array_sized_new(blocksize);
		g_byte_array_set_size(buf, blocksize);
		if (-1 == (r = net_read(fd, buf->data, blocksize))) {
			g_byte_array_free(buf, TRUE);
			switch (errno) {
			case EAGAIN:
#if EWOULDBLOCK != EAGAIN
			case EWOULDBLOCK:
#endif
				return len ? NETWORK_STATUS_SUCCESS : NETWORK_STATUS_WAIT_FOR_EVENT;
			case ECONNRESET:
				return NETWORK_STATUS_CONNECTION_CLOSE;
			default:
				VR_ERROR(vr, "oops, read from fd=%d failed: %s", fd, g_strerror(errno) );
				return NETWORK_STATUS_FATAL_ERROR;
			}
		} else if (0 == r) {
			g_byte_array_free(buf, TRUE);
			return len ? NETWORK_STATUS_SUCCESS : NETWORK_STATUS_CONNECTION_CLOSE;
		}
		g_byte_array_set_size(buf, r);
		chunkqueue_append_bytearr(cq, buf);
		len += r;
	} while (r == blocksize && len < max_read);

	return NETWORK_STATUS_SUCCESS;
}

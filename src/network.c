
#include "network.h"

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

network_status_t network_write(connection *con, int fd, chunkqueue *cq) {
	network_status_t res;
#ifdef TCP_CORK
	int corked = 0;
#endif

#ifdef TCP_CORK
	/* Linux: put a cork into the socket as we want to combine the write() calls
	 * but only if we really have multiple chunks
	 */
	if (cq->queue->length > 1) {
		corked = 1;
		setsockopt(fd, IPPROTO_TCP, TCP_CORK, &corked, sizeof(corked));
	}
#endif

	/* res = network_write_writev(con, fd, cq); */
	res = network_write_sendfile(con, fd, cq);

#ifdef TCP_CORK
	if (corked) {
		corked = 0;
		setsockopt(fd, IPPROTO_TCP, TCP_CORK, &corked, sizeof(corked));
	}
#endif

	return res;
}

network_status_t network_read(connection *con, int fd, chunkqueue *cq) {
	const ssize_t blocksize = 16*1024; /* 16k */
	const off_t max_read = 16 * blocksize; /* 256k */
	ssize_t r;
	off_t len = 0;

	do {
		GString *buf = g_string_sized_new(blocksize);
		g_string_set_size(buf, blocksize);
		if (-1 == (r = net_read(fd, buf->str, blocksize))) {
			g_string_free(buf, TRUE);
			switch (errno) {
			case EAGAIN:
#if EWOULDBLOCK != EAGAIN
			case EWOULDBLOCK:
#endif
				return len ? NETWORK_STATUS_SUCCESS : NETWORK_STATUS_WAIT_FOR_EVENT;
			case ECONNRESET:
				return NETWORK_STATUS_CONNECTION_CLOSE;
			default:
				CON_ERROR(con, "oops, read from fd=%d failed: %s", fd, g_strerror(errno) );
				return NETWORK_STATUS_FATAL_ERROR;
			}
		} else if (0 == r) {
			g_string_free(buf, TRUE);
			return len ? NETWORK_STATUS_SUCCESS : NETWORK_STATUS_CONNECTION_CLOSE;
		}
		g_string_truncate(buf, r);
		chunkqueue_append_string(cq, buf);
		len += r;
	} while (r == blocksize && len < max_read);

	return NETWORK_STATUS_SUCCESS;
}

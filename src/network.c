
#include "network.h"

/** repeats write after EINTR */
static ssize_t net_write(int fd, void *buf, ssize_t nbyte) {
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

network_status_t network_write(server *srv, connection *con, int fd, chunkqueue *cq) {
	const ssize_t blocksize = 16*1024; /* 16k */
	const off_t max_write = 16 * blocksize; /* 256k */
	char *block_data;
	off_t block_len;
	ssize_t r;
	off_t len = 0;
	chunkiter ci;

	do {
		if (0 == cq->length) return NETWORK_STATUS_SUCCESS;

		ci = chunkqueue_iter(cq);
		switch (chunkiter_read(srv, con, ci, 0, blocksize, &block_data, &block_len)) {
		case HANDLER_GO_ON:
			break;
		case HANDLER_WAIT_FOR_FD:
			return len ? NETWORK_STATUS_SUCCESS : NETWORK_STATUS_WAIT_FOR_EVENT;
		case HANDLER_ERROR:
		default:
			return NETWORK_STATUS_FATAL_ERROR;
		}

		if (-1 == (r = net_write(fd, block_data, block_len))) {
			switch (errno) {
			case EAGAIN:
#if EWOULDBLOCK != EAGAIN
			case EWOULDBLOCK
#endif
				return len ? NETWORK_STATUS_SUCCESS : NETWORK_STATUS_WAIT_FOR_EVENT;
			case ECONNRESET:
				return NETWORK_STATUS_CONNECTION_CLOSE;
			default:
				CON_ERROR(srv, con, "oops, read from fd=%d failed: %s (%d)", fd, strerror(errno), errno );
				return NETWORK_STATUS_FATAL_ERROR;
			}
		} else if (0 == r) {
			return len ? NETWORK_STATUS_SUCCESS : NETWORK_STATUS_WAIT_FOR_EVENT;
		}
		chunkqueue_skip(cq, r);
		len += r;
	} while (r == block_len && len < max_write);

	return NETWORK_STATUS_SUCCESS;
}

/** repeats read after EINTR */
static ssize_t net_read(int fd, void *buf, ssize_t nbyte) {
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

network_status_t network_read(server *srv, connection *con, int fd, chunkqueue *cq) {
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
			case EWOULDBLOCK
#endif
				return len ? NETWORK_STATUS_SUCCESS : NETWORK_STATUS_WAIT_FOR_EVENT;
			case ECONNRESET:
				return NETWORK_STATUS_CONNECTION_CLOSE;
			default:
				CON_ERROR(srv, con, "oops, read from fd=%d failed: %s (%d)", fd, strerror(errno), errno );
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


#include "network.h"

network_status_t network_backend_write(server *srv, connection *con, int fd, chunkqueue *cq) {
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
			case EPIPE:
				return NETWORK_STATUS_CONNECTION_CLOSE;
			default:
				CON_ERROR(srv, con, "oops, write to fd=%d failed: %s (%d)", fd, strerror(errno), errno );
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

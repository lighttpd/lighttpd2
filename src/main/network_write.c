
#include <lighttpd/base.h>

liNetworkStatus li_network_backend_write(liVRequest *vr, int fd, liChunkQueue *cq, goffset *write_max) {
	const ssize_t blocksize = 16*1024; /* 16k */
	char *block_data;
	off_t block_len;
	ssize_t r;
	gboolean did_write_something = FALSE;
	liChunkIter ci;

	do {
		if (0 == cq->length)
			return did_write_something ? LI_NETWORK_STATUS_SUCCESS : LI_NETWORK_STATUS_FATAL_ERROR;

		ci = li_chunkqueue_iter(cq);
		switch (li_chunkiter_read(vr, ci, 0, blocksize, &block_data, &block_len)) {
		case LI_HANDLER_GO_ON:
			break;
		case LI_HANDLER_ERROR:
		default:
			return LI_NETWORK_STATUS_FATAL_ERROR;
		}

		if (-1 == (r = li_net_write(fd, block_data, block_len))) {
			switch (errno) {
			case EAGAIN:
#if EWOULDBLOCK != EAGAIN
			case EWOULDBLOCK:
#endif
				return did_write_something ? LI_NETWORK_STATUS_SUCCESS : LI_NETWORK_STATUS_WAIT_FOR_EVENT;
			case ECONNRESET:
			case EPIPE:
			case ETIMEDOUT:
				return LI_NETWORK_STATUS_CONNECTION_CLOSE;
			default:
				VR_ERROR(vr, "oops, write to fd=%d failed: %s", fd, g_strerror(errno));
				return LI_NETWORK_STATUS_FATAL_ERROR;
			}
		} else if (0 == r) {
			return did_write_something ? LI_NETWORK_STATUS_SUCCESS : LI_NETWORK_STATUS_WAIT_FOR_EVENT;
		}

		li_chunkqueue_skip(cq, r);
		did_write_something = TRUE;
		*write_max -= r;
	} while (r == block_len && *write_max > 0);

	return LI_NETWORK_STATUS_SUCCESS;
}

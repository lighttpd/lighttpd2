
#include "base.h"

network_status_t network_backend_write(vrequest *vr, int fd, chunkqueue *cq, goffset *write_max) {
	const ssize_t blocksize = 16*1024; /* 16k */
	char *block_data;
	off_t block_len;
	ssize_t r;
	gboolean did_write_something = FALSE;
	chunkiter ci;
	worker *wrk;
	ev_tstamp ts;

	do {
		if (0 == cq->length)
			return did_write_something ? NETWORK_STATUS_SUCCESS : NETWORK_STATUS_FATAL_ERROR;

		ci = chunkqueue_iter(cq);
		switch (chunkiter_read(vr, ci, 0, blocksize, &block_data, &block_len)) {
		case HANDLER_GO_ON:
			break;
		case HANDLER_WAIT_FOR_FD:
			return did_write_something ? NETWORK_STATUS_SUCCESS : NETWORK_STATUS_WAIT_FOR_FD;
		case HANDLER_ERROR:
		default:
			return NETWORK_STATUS_FATAL_ERROR;
		}

		if (-1 == (r = net_write(fd, block_data, block_len))) {
			switch (errno) {
			case EAGAIN:
#if EWOULDBLOCK != EAGAIN
			case EWOULDBLOCK:
#endif
				return did_write_something ? NETWORK_STATUS_SUCCESS : NETWORK_STATUS_WAIT_FOR_EVENT;
			case ECONNRESET:
			case EPIPE:
				return NETWORK_STATUS_CONNECTION_CLOSE;
			default:
				VR_ERROR(vr, "oops, write to fd=%d failed: %s", fd, g_strerror(errno));
				return NETWORK_STATUS_FATAL_ERROR;
			}
		} else if (0 == r) {
			return did_write_something ? NETWORK_STATUS_SUCCESS : NETWORK_STATUS_WAIT_FOR_EVENT;
		}
		chunkqueue_skip(cq, r);
		did_write_something = TRUE;
		*write_max -= r;

		/* stats */
		wrk = vr->con->wrk;
		vr->con->wrk->stats.bytes_out += r;
		vr->con->stats.bytes_out += r;

		/* update 5s stats */
		ts = CUR_TS(wrk);

		if ((ts - vr->con->stats.last_avg) > 5) {
			vr->con->stats.bytes_out_5s_diff = vr->con->stats.bytes_out - vr->con->stats.bytes_out_5s;
			vr->con->stats.bytes_out_5s = vr->con->stats.bytes_out;
			vr->con->stats.last_avg = ts;
		}

	} while (r == block_len && *write_max > 0);

	return NETWORK_STATUS_SUCCESS;
}

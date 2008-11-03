
#include "base.h"

/* first chunk must be a FILE_CHUNK ! */
network_status_t network_backend_sendfile(vrequest *vr, int fd, chunkqueue *cq, goffset *write_max) {
	off_t file_offset, toSend;
	ssize_t r;
	gboolean did_write_something = FALSE;
	chunkiter ci;
	chunk *c;
	worker *wrk;
	time_t ts;

	if (0 == cq->length) return NETWORK_STATUS_FATAL_ERROR;

	do {
		ci = chunkqueue_iter(cq);

		if (FILE_CHUNK != (c = chunkiter_chunk(ci))->type) {
			return did_write_something ? NETWORK_STATUS_SUCCESS : NETWORK_STATUS_FATAL_ERROR;
		}

		switch (chunkfile_open(vr, c->file.file)) {
		case HANDLER_GO_ON:
			break;
		case HANDLER_WAIT_FOR_FD:
			return NETWORK_STATUS_WAIT_FOR_FD;
		default:
			return NETWORK_STATUS_FATAL_ERROR;
		}

		file_offset = c->offset + c->file.start;
		toSend = c->file.length - c->offset;
		if (toSend > *write_max) toSend = *write_max;

		while (-1 == (r = sendfile(fd, c->file.file->fd, &file_offset, toSend))) {
			switch (errno) {
			case EAGAIN:
#if EWOULDBLOCK != EAGAIN
			case EWOULDBLOCK
#endif
				return did_write_something ? NETWORK_STATUS_SUCCESS : NETWORK_STATUS_WAIT_FOR_EVENT;
			case ECONNRESET:
			case EPIPE:
				return NETWORK_STATUS_CONNECTION_CLOSE;
			case EINTR:
				break; /* try again */
			case EINVAL:
			case ENOSYS:
				/* TODO: print a warning? */
				NETWORK_FALLBACK(network_backend_write, write_max);
				return NETWORK_STATUS_SUCCESS;
			default:
				VR_ERROR(vr, "oops, write to fd=%d failed: %s", fd, g_strerror(errno));
				return NETWORK_STATUS_FATAL_ERROR;
			}
		}
		if (0 == r) {
			/* don't care about cached stat - file is open */
			struct stat st;
			if (-1 == fstat(fd, &st)) {
				VR_ERROR(vr, "Couldn't fstat file: %s", g_strerror(errno));
				return NETWORK_STATUS_FATAL_ERROR;
			}

			if (file_offset > st.st_size) {
				/* file shrinked, close the connection */
				VR_ERROR(vr, "%s", "File shrinked, aborting");
				return NETWORK_STATUS_FATAL_ERROR;
			}
			return did_write_something ? NETWORK_STATUS_SUCCESS : NETWORK_STATUS_WAIT_FOR_EVENT;
		}
		chunkqueue_skip(cq, r);
		*write_max -= r;
		did_write_something = TRUE;

		/* stats */
		wrk = vr->con->wrk;
		wrk->stats.bytes_out += r;
		vr->con->stats.bytes_out += r;

		/* update 5s stats */
		ts = CUR_TS(wrk);

		if ((ts - vr->con->stats.last_avg) > 5) {
			vr->con->stats.bytes_out_5s_diff = vr->con->wrk->stats.bytes_out - vr->con->wrk->stats.bytes_out_5s;
			vr->con->stats.bytes_out_5s = vr->con->stats.bytes_out;
			vr->con->stats.last_avg = ts;
		}

		if (0 == cq->length) return NETWORK_STATUS_SUCCESS;
	} while (r == toSend && *write_max > 0);

	return NETWORK_STATUS_SUCCESS;
}

network_status_t network_write_sendfile(vrequest *vr, int fd, chunkqueue *cq) {
	goffset write_max = 256*1024; // 256kB //;
	if (cq->length == 0) return NETWORK_STATUS_FATAL_ERROR;
	do {
		switch (chunkqueue_first_chunk(cq)->type) {
		case MEM_CHUNK:
			NETWORK_FALLBACK(network_backend_writev, &write_max);
			break;
		case FILE_CHUNK:
			NETWORK_FALLBACK(network_backend_sendfile, &write_max);
			break;
		default:
			return NETWORK_STATUS_FATAL_ERROR;
		}
		if (cq->length == 0) return NETWORK_STATUS_SUCCESS;
	} while (write_max > 0);
	return NETWORK_STATUS_SUCCESS;
}

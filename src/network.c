
#include "base.h"

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

network_status_t network_write(vrequest *vr, int fd, chunkqueue *cq) {
	network_status_t res;
	ev_tstamp now = CUR_TS(vr->con->wrk);
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
	res = network_write_sendfile(vr, fd, cq);

#ifdef TCP_CORK
	if (corked) {
		corked = 0;
		setsockopt(fd, IPPROTO_TCP, TCP_CORK, &corked, sizeof(corked));
	}
#endif

	/* only update once a second, the cast is to round the timestamp */
	if ((vr->con->io_timeout_elem.ts + 1.) < now)
		waitqueue_push(&vr->con->wrk->io_timeout_queue, &vr->con->io_timeout_elem);

	return res;
}

network_status_t network_read(vrequest *vr, int fd, chunkqueue *cq) {
	const ssize_t blocksize = 16*1024; /* 16k */
	const off_t max_read = 16 * blocksize; /* 256k */
	ssize_t r;
	off_t len = 0;
	worker *wrk = vr->con->wrk;
	ev_tstamp now = CUR_TS(wrk);

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
				VR_ERROR(vr, "oops, read from fd=%d failed: %s", fd, g_strerror(errno) );
				return NETWORK_STATUS_FATAL_ERROR;
			}
		} else if (0 == r) {
			g_string_free(buf, TRUE);
			return len ? NETWORK_STATUS_SUCCESS : NETWORK_STATUS_CONNECTION_CLOSE;
		}
		g_string_truncate(buf, r);
		chunkqueue_append_string(cq, buf);
		len += r;

		/* stats */
		wrk = vr->con->wrk;
		wrk->stats.bytes_in += r;
		vr->con->stats.bytes_in += r;

		/* update 5s stats */

		if ((now - vr->con->stats.last_avg) >= 5.0) {
			vr->con->stats.bytes_in_5s_diff = vr->con->stats.bytes_in - vr->con->stats.bytes_in_5s;
			vr->con->stats.bytes_in_5s = vr->con->stats.bytes_in;
			vr->con->stats.last_avg = now;
		}
	} while (r == blocksize && len < max_read);

	/* only update once a second, the cast is to round the timestamp */
	if ((vr->con->io_timeout_elem.ts + 1.) < now)
		waitqueue_push(&vr->con->wrk->io_timeout_queue, &vr->con->io_timeout_elem);

	return NETWORK_STATUS_SUCCESS;
}

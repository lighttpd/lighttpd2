
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

network_status_t network_write(vrequest *vr, int fd, chunkqueue *cq) {
	network_status_t res;
	ev_tstamp ts, now = CUR_TS(vr->con->wrk);
	worker *wrk;
#ifdef TCP_CORK
	int corked = 0;
#endif
	goffset write_max = 256*1024, write_bytes, wrote; /* 256 kb */

	if (CORE_OPTION(CORE_OPTION_THROTTLE).number) {
		/* throttling is enabled */
		if (G_UNLIKELY((now - vr->con->throttle.ts) > vr->con->wrk->throttle_queue.delay)) {
			vr->con->throttle.magazine += CORE_OPTION(CORE_OPTION_THROTTLE).number * (now - vr->con->throttle.ts);
			if (vr->con->throttle.magazine > CORE_OPTION(CORE_OPTION_THROTTLE).number)
				vr->con->throttle.magazine = CORE_OPTION(CORE_OPTION_THROTTLE).number;
			vr->con->throttle.ts = now;
			/*g_print("throttle magazine: %u kbytes\n", vr->con->throttle.magazine / 1024);*/
		}
		write_max = vr->con->throttle.magazine;
	}

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

	vr->con->throttle.magazine = write_bytes;
	/* check if throttle magazine is empty */
	if (CORE_OPTION(CORE_OPTION_THROTTLE).number && write_bytes == 0) {
		/* remove EV_WRITE from sockwatcher for now */
		ev_io_rem_events(vr->con->wrk->loop, &vr->con->sock_watcher, EV_WRITE);
		waitqueue_push(&vr->con->wrk->throttle_queue, &vr->con->throttle.queue_elem);
		return NETWORK_STATUS_WAIT_FOR_AIO_EVENT;
	}

	/* stats */
	wrk = vr->con->wrk;
	wrk->stats.bytes_out += wrote;
	vr->con->stats.bytes_out += wrote;

	/* update 5s stats */
	ts = CUR_TS(wrk);

	if ((ts - vr->con->stats.last_avg) >= 5.0) {
		vr->con->stats.bytes_out_5s_diff = vr->con->wrk->stats.bytes_out - vr->con->wrk->stats.bytes_out_5s;
		vr->con->stats.bytes_out_5s = vr->con->stats.bytes_out;
		vr->con->stats.last_avg = ts;
	}

	/* only update once a second, the cast is to round the timestamp */
	if ((vr->con->io_timeout_elem.ts + 1.) < now)
		waitqueue_push(&vr->con->wrk->io_timeout_queue, &vr->con->io_timeout_elem);

	return res;
}

network_status_t network_read(vrequest *vr, int fd, chunkqueue *cq) {
	const ssize_t blocksize = 16*1024; /* 16k */
	off_t max_read = 16 * blocksize; /* 256k */
	ssize_t r;
	off_t len = 0;
	worker *wrk = vr->con->wrk;
	ev_tstamp now = CUR_TS(wrk);

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

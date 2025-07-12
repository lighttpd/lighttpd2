
#include <lighttpd/base.h>
#include <lighttpd/throttle.h>

#include <netinet/tcp.h>
#include <sys/socket.h>

void li_stream_simple_socket_close(liIOStream *stream, gboolean aborted) {
	int fd = li_event_io_fd(&stream->io_watcher);

	li_event_detach(&stream->io_watcher);

	if (-1 == fd) return;

	stream->out_closed = stream->in_closed = TRUE;
	stream->can_read = stream->can_write = FALSE;
	if (NULL != stream->stream_in.out) {
		stream->stream_in.out->is_closed = TRUE;
	}

	if (aborted || stream->in_closed) {
		fd = li_iostream_reset(stream);
		if (-1 != fd) {
			shutdown(fd, SHUT_RDWR);
			close(fd);
		}
	} else {
		liWorker *wrk = li_worker_from_iostream(stream);
		li_event_clear(&stream->io_watcher); /* sets io_fd to -1 */

		shutdown(fd, SHUT_WR);
		li_stream_disconnect(&stream->stream_out);
		li_worker_add_closing_socket(wrk, fd);
	}
	LI_FORCE_ASSERT(-1 == li_event_io_fd(&stream->io_watcher));
}

static void stream_simple_socket_read_throttle_notify(liThrottleState *state, gpointer data) {
	liIOStream *stream = data;
	UNUSED(state);
	stream->throttled_in = FALSE;
	stream->can_read = TRUE;
	li_stream_again(&stream->stream_out);
}
static void stream_simple_socket_read(liIOStream *stream, liBuffer **buffer) {
	liNetworkStatus res;
	GError *err = NULL;
	liWorker *wrk = li_worker_from_iostream(stream);
	int fd = li_event_io_fd(&stream->io_watcher);
	off_t max_read = 256 * 1024; /* 256k */
	liChunkQueue *raw_in = stream->stream_in.out;

	if (NULL != stream->throttle_in) {
		max_read = li_throttle_query(wrk, stream->throttle_in, max_read, stream_simple_socket_read_throttle_notify, stream);
		if (0 == max_read) {
			stream->throttled_in = TRUE;
			return;
		}
	}

	if (NULL == *buffer && NULL != wrk->network_read_buf) {
		/* reuse worker buf if needed */
		*buffer = wrk->network_read_buf;
		wrk->network_read_buf = NULL;
	}

	{
		goffset current_in_bytes = raw_in->bytes_in;
		res = li_network_read(fd, raw_in, max_read, buffer, &err);
		if (NULL != stream->throttle_in) {
			li_throttle_update(stream->throttle_in, raw_in->bytes_in - current_in_bytes);
		}
	}

	if (NULL == wrk->network_read_buf && NULL != *buffer
		&& 1 == g_atomic_int_get(&((liBuffer*)*buffer)->refcount)) {
		/* move buffer back to worker if we didn't use it */
		wrk->network_read_buf = *buffer;
		*buffer = NULL;
	}

	switch (res) {
	case LI_NETWORK_STATUS_SUCCESS:
		break;
	case LI_NETWORK_STATUS_FATAL_ERROR:
		ERROR(wrk->srv, "network read fatal error: %s", NULL != err ? err->message : "(unknown)");
		g_error_free(err);
		li_stream_simple_socket_close(stream, TRUE);
		break;
	case LI_NETWORK_STATUS_CONNECTION_CLOSE:
		li_event_io_rem_events(&stream->io_watcher, LI_EV_READ);
		stream->stream_in.out->is_closed = TRUE;
		stream->in_closed = TRUE;
		stream->can_read = FALSE;
		break;
	case LI_NETWORK_STATUS_WAIT_FOR_EVENT:
		stream->can_read = FALSE;
		break;
	}
}

static void stream_simple_socket_write_throttle_notify(liThrottleState *state, gpointer data) {
	liIOStream *stream = data;
	UNUSED(state);
	stream->throttled_out = FALSE;
	stream->can_write = TRUE;
	li_stream_again(&stream->stream_out);
}
static void stream_simple_socket_write(liIOStream *stream) {
	liNetworkStatus res;
	liChunkQueue *raw_out = stream->stream_out.out;
	liChunkQueue *from = (NULL != stream->stream_out.source) ? stream->stream_out.source->out : NULL;
	int fd = li_event_io_fd(&stream->io_watcher);
	liWorker *wrk = li_worker_from_iostream(stream);

	if (NULL != from) li_chunkqueue_steal_all(raw_out, from);

	if (raw_out->length > 0) {
		static const goffset WRITE_MAX = 256*1024; /* 256kB */
		goffset write_max, current_out_bytes = raw_out->bytes_out;
		GError *err = NULL;

		write_max = MAX(WRITE_MAX, raw_out->length);
		if (NULL != stream->throttle_out) {
			write_max = li_throttle_query(wrk, stream->throttle_out, write_max, stream_simple_socket_write_throttle_notify, stream);
			if (0 == write_max) {
				stream->throttled_out = TRUE;
				return;
			}
		}

		res = li_network_write(fd, raw_out, write_max, &err);

		if (NULL != stream->throttle_out) {
			li_throttle_update(stream->throttle_out, raw_out->bytes_out - current_out_bytes);
		}

		switch (res) {
		case LI_NETWORK_STATUS_SUCCESS:
			break;
		case LI_NETWORK_STATUS_FATAL_ERROR:
			ERROR(wrk->srv, "network write fatal error: %s", NULL != err ? err->message : "(unknown)");
			g_error_free(err);
			li_stream_simple_socket_close(stream, TRUE);
			break;
		case LI_NETWORK_STATUS_CONNECTION_CLOSE:
			li_stream_simple_socket_close(stream, TRUE);
			break;
		case LI_NETWORK_STATUS_WAIT_FOR_EVENT:
			stream->can_write = FALSE;
			break;
		}
	}

	if (0 == raw_out->length && raw_out->is_closed) {
		fd = li_event_io_fd(&stream->io_watcher);
		li_event_io_rem_events(&stream->io_watcher, LI_EV_WRITE);
		if (-1 != fd) shutdown(fd, SHUT_WR);
		stream->out_closed = TRUE;
		stream->can_write = FALSE;
		li_stream_disconnect(&stream->stream_out);
	}
}

void li_stream_simple_socket_io_cb(liIOStream *stream, liIOStreamEvent event) {
	li_stream_simple_socket_io_cb_with_buffer(stream, event, (liBuffer**) &stream->data);
}

void li_stream_simple_socket_io_cb_with_buffer(liIOStream *stream, liIOStreamEvent event, liBuffer **buffer) {
	switch (event) {
	case LI_IOSTREAM_READ:
		stream_simple_socket_read(stream, buffer);
		break;
	case LI_IOSTREAM_WRITE:
		stream_simple_socket_write(stream);
		break;
	case LI_IOSTREAM_DESTROY:
		if (NULL != *buffer) {
			li_buffer_release(*buffer);
			*buffer = NULL;
		}
	default:
		break;
	}
}

void li_stream_simple_socket_flush(liIOStream *stream) {
	int val = 1;
	int fd = li_event_io_fd(&stream->io_watcher);
	if (-1 != fd) {
		/* setting TCP_NODELAY should flush the socket. if it fails it probably isn't a TCP socket,
		 * so no need to disable TCP_NODELAY */
		if (-1 != setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &val, sizeof(val))) {
			val = 0;
			setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &val, sizeof(val));
		}
	}
}

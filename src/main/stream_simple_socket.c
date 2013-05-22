
#include <lighttpd/base.h>

void li_stream_simple_socket_close(liIOStream *stream, gboolean aborted) {
	int fd = stream->io_watcher.fd;

	ev_io_stop(stream->wrk->loop, &stream->io_watcher);

	if (-1 == fd) return;

	stream->out_closed = stream->in_closed = TRUE;
	stream->can_read = stream->can_write = FALSE;
	if (NULL != stream->stream_in.out) {
		stream->stream_in.out->is_closed = TRUE;
	}

	if (aborted || stream->in_closed) {
		li_iostream_acquire(stream);
		fd = li_iostream_reset(stream);
		if (-1 != fd) {
			shutdown(fd, SHUT_RDWR);
			close(fd);
		}
	} else {
		stream->io_watcher.fd = -1;

		shutdown(fd, SHUT_WR);
		li_stream_disconnect(&stream->stream_out);
		li_worker_add_closing_socket(stream->wrk, fd);
	}
}

static void stream_simple_socket_read(liIOStream *stream, gpointer *data) {
	liNetworkStatus res;
	GError *err = NULL;
	liWorker *wrk = stream->wrk;

	liChunkQueue *raw_in = stream->stream_in.out;

	if (NULL == *data && NULL != wrk->network_read_buf) {
		/* reuse worker buf if needed */
		*data = wrk->network_read_buf;
		wrk->network_read_buf = NULL;
	}

	{
		liBuffer *raw_in_buffer = *data;
		res = li_network_read(stream->io_watcher.fd, raw_in, &raw_in_buffer, &err);
		*data = raw_in_buffer;
	}

	if (NULL == wrk->network_read_buf && NULL != *data
		&& 1 == g_atomic_int_get(&((liBuffer*)*data)->refcount)) {
		/* move buffer back to worker if we didn't use it */
		wrk->network_read_buf = *data;
		*data = NULL;
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
		li_ev_io_rem_events(stream->wrk->loop, &stream->io_watcher, EV_READ);
		stream->stream_in.out->is_closed = TRUE;
		stream->in_closed = TRUE;
		stream->can_read = FALSE;
		break;
	case LI_NETWORK_STATUS_WAIT_FOR_EVENT:
		stream->can_read = FALSE;
		break;
	}
}

static void stream_simple_socket_write(liIOStream *stream) {
	liNetworkStatus res;
	liChunkQueue *raw_out = stream->stream_out.out;
	liChunkQueue *from = stream->stream_out.source->out;

	li_chunkqueue_steal_all(raw_out, from);

	if (raw_out->length > 0) {
		static const goffset WRITE_MAX = 256*1024; /* 256kB */
		goffset write_max;
		GError *err = NULL;

		write_max = WRITE_MAX;

		res = li_network_write(stream->io_watcher.fd, raw_out, write_max, &err);

		switch (res) {
		case LI_NETWORK_STATUS_SUCCESS:
			break;
		case LI_NETWORK_STATUS_FATAL_ERROR:
			ERROR(stream->wrk->srv, "network write fatal error: %s", NULL != err ? err->message : "(unknown)");
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
		int fd = stream->io_watcher.fd;
		li_ev_io_rem_events(stream->wrk->loop, &stream->io_watcher, EV_WRITE);
		if (-1 != fd) shutdown(fd, SHUT_WR);
		stream->out_closed = TRUE;
		stream->can_write = FALSE;
		li_stream_disconnect(&stream->stream_out);
	}
}

void li_stream_simple_socket_io_cb(liIOStream *stream, liIOStreamEvent event) {
	li_stream_simple_socket_io_cb_with_context(stream, event, &stream->data);
}

void li_stream_simple_socket_io_cb_with_context(liIOStream *stream, liIOStreamEvent event, gpointer *data) {
	switch (event) {
	case LI_IOSTREAM_READ:
		stream_simple_socket_read(stream, data);
		break;
	case LI_IOSTREAM_WRITE:
		stream_simple_socket_write(stream);
		break;
	case LI_IOSTREAM_DESTROY:
		if (NULL != *data) {
			li_buffer_release(*data);
			*data = NULL;
		}
	default:
		break;
	}
}

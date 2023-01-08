
#include <lighttpd/base.h>
#include <lighttpd/throttle.h>

const gchar* li_stream_event_string(liStreamEvent event) {
	switch (event) {
	case LI_STREAM_NEW_DATA:
		return "new_data";
	case LI_STREAM_NEW_CQLIMIT:
		return "new_cqlimit";
	case LI_STREAM_CONNECTED_DEST:
		return "connected_dest";
	case LI_STREAM_CONNECTED_SOURCE:
		return "connected_source";
	case LI_STREAM_DISCONNECTED_DEST:
		return "disconnected_dest";
	case LI_STREAM_DISCONNECTED_SOURCE:
		return "disconnected_source";
	case LI_STREAM_DESTROY:
		return "destroy";
	}
	return "invalid stream event";
}

const gchar* li_iostream_event_string(liIOStreamEvent event) {
	switch (event) {
	case LI_IOSTREAM_READ:
		return "read";
	case LI_IOSTREAM_WRITE:
		return "write";
	case LI_IOSTREAM_CONNECTED_DEST:
		return "connected_dest";
	case LI_IOSTREAM_CONNECTED_SOURCE:
		return "connected_source";
	case LI_IOSTREAM_DISCONNECTED_DEST:
		return "disconnected_dest";
	case LI_IOSTREAM_DISCONNECTED_SOURCE:
		return "disconnected_source";
	case LI_IOSTREAM_DESTROY:
		return "destroy";
	}
	return "invalid stream event";

}

/* callback can assume that the stream is not destroyed while the callback is running */
static void li_stream_safe_cb(liStream *stream, liStreamEvent event) {
	if (NULL != stream->cb) {
		li_stream_acquire(stream);
		stream->cb(stream, event);
		li_stream_release(stream);
	}
}

static void stream_new_data_job_cb(liJob *job) {
	liStream *stream = LI_CONTAINER_OF(job, liStream, new_data_job);
	li_stream_safe_cb(stream, LI_STREAM_NEW_DATA);
}

void li_stream_init(liStream* stream, liEventLoop *loop, liStreamCB cb) {
	stream->refcount = 1;
	stream->source = stream->dest = NULL;
	stream->out = li_chunkqueue_new();
	li_job_init(&stream->new_data_job, stream_new_data_job_cb);
	stream->loop = loop;
	stream->cb = cb;
}

void li_stream_acquire(liStream* stream) {
	LI_FORCE_ASSERT(g_atomic_int_get(&stream->refcount) > 0);
	g_atomic_int_inc(&stream->refcount);
}

void li_stream_release(liStream* stream) {
	LI_FORCE_ASSERT(g_atomic_int_get(&stream->refcount) > 0);
	if (g_atomic_int_dec_and_test(&stream->refcount)) {
		li_job_clear(&stream->new_data_job);
		li_chunkqueue_free(stream->out);
		stream->out = NULL;
		if (NULL != stream->cb) stream->cb(stream, LI_STREAM_DESTROY); /* "unsafe" cb, we can't keep a ref this time */
	}
}

void li_stream_connect(liStream *source, liStream *dest) {
	/* streams must be "valid" */
	LI_FORCE_ASSERT(source->refcount > 0 && dest->refcount > 0);

	LI_FORCE_ASSERT(NULL == source->dest && NULL == dest->source);
	if (NULL != source->dest || NULL != dest->source) {
		g_error("Can't connect already connected streams");
	}

	/* keep them alive for this function and for callbacks (-> callbacks are "safe") */
	g_atomic_int_inc(&source->refcount);
	g_atomic_int_inc(&dest->refcount);

	/* references for the links */
	g_atomic_int_inc(&source->refcount);
	g_atomic_int_inc(&dest->refcount);
	source->dest = dest;
	dest->source = source;

	if (NULL != source->cb) source->cb(source, LI_STREAM_CONNECTED_DEST);
	/* only notify dest if source didn't disconnect */
	if (source->dest == dest && NULL != dest->cb) dest->cb(dest, LI_STREAM_CONNECTED_SOURCE);

	/* still connected: sync liCQLimit */
	if (source->dest == dest) {
		liCQLimit *sl = source->out->limit, *dl = dest->out->limit;
		if (sl != NULL && dl == NULL) {
			li_stream_set_cqlimit(dest, NULL, sl);
		}
		else if (sl == NULL && dl != NULL) {
			li_stream_set_cqlimit(NULL, source, dl);
		}
	}

	/* still connected and source has data: notify dest */
	if (source->dest == dest && (source->out->length > 0 || source->out->is_closed)) {
		li_stream_again_later(dest);
	}

	/* release our "function" refs */
	li_stream_release(source);
	li_stream_release(dest);
}

static void _disconnect(liStream *source, liStream *dest) {
	/* streams must be "valid" */
	LI_FORCE_ASSERT(g_atomic_int_get(&source->refcount) > 0 && g_atomic_int_get(&dest->refcount) > 0);
	LI_FORCE_ASSERT(source->dest == dest && dest->source == source);

	source->dest = NULL;
	dest->source = NULL;
	/* we still have the references from the links -> callbacks are "safe" */
	if (NULL != source->cb) source->cb(source, LI_STREAM_DISCONNECTED_DEST);
	if (NULL != dest->cb) dest->cb(dest, LI_STREAM_DISCONNECTED_SOURCE);

	/* release references from the link */
	li_stream_release(source);
	li_stream_release(dest);
}

void li_stream_disconnect(liStream *stream) {
	if (NULL == stream || NULL == stream->source) return;
	_disconnect(stream->source, stream);
}

void li_stream_disconnect_dest(liStream *stream) {
	if (NULL == stream || NULL == stream->dest) return;
	_disconnect(stream, stream->dest);
}

void li_stream_reset(liStream *stream) {
	if (NULL == stream || 0 == stream->refcount) return;

	li_stream_acquire(stream);
	if (NULL != stream->source) _disconnect(stream->source, stream);
	if (NULL != stream->dest) _disconnect(stream, stream->dest);
	li_stream_release(stream);
}

void li_stream_notify(liStream *stream) {
	if (NULL != stream->dest) li_stream_again(stream->dest);
}

void li_stream_notify_later(liStream *stream) {
	if (NULL != stream->dest) li_stream_again_later(stream->dest);
}

void li_stream_again(liStream *stream) {
	if (NULL != stream->loop) {
		li_job_now(&stream->loop->jobqueue, &stream->new_data_job);
	}
}

void li_stream_again_later(liStream *stream) {
	if (NULL != stream->loop) {
		li_job_later(&stream->loop->jobqueue, &stream->new_data_job);
	}
}

void li_stream_detach(liStream *stream) {
	stream->loop = NULL;
	li_job_stop(&stream->new_data_job);

	li_chunkqueue_set_limit(stream->out, NULL);
}

void li_stream_attach(liStream *stream, liEventLoop *loop) {
	stream->loop = loop;
	li_stream_again_later(stream);
}

void li_stream_set_cqlimit(liStream *first, liStream *last, liCQLimit *limit) {
	if (NULL != limit) li_cqlimit_acquire(limit);
	if (NULL == first) {
		while (NULL != last && NULL == last->out->limit) {
			if (limit == last->out->limit) break;
			li_chunkqueue_set_limit(last->out, limit);
			if (NULL != last->cb) {
				liStream *cur = last;
				last = last->source;
				li_stream_acquire(cur);
				cur->cb(cur, LI_STREAM_NEW_CQLIMIT);
				li_stream_release(cur);
			} else {
				last = last->source;
			}
		}
	} else {
		gboolean reached_last = FALSE;
		while (NULL != first && !reached_last && NULL != first->out->limit) {
			if (limit == first->out->limit) break;
			if (first == last) reached_last = TRUE;
			li_chunkqueue_set_limit(first->out, limit);
			if (NULL != first->cb) {
				liStream *cur = first;
				first = first->dest;
				li_stream_acquire(cur);
				cur->cb(cur, LI_STREAM_NEW_CQLIMIT);
				li_stream_release(cur);
			} else {
				first = first->dest;
			}
		}
	}
	if (NULL != limit) li_cqlimit_release(limit);
}

gboolean li_streams_empty(liStream *first, liStream *last) {
	if (NULL == first) {
		while (NULL != last) {
			if (NULL != last->out && last->out->length > 0) return FALSE;
			last = last->source;
		}
	} else {
		while (NULL != first) {
			if (NULL != first->out && first->out->length > 0) return FALSE;
			if (first == last) break;
			first = first->dest;
		}
	}
	return TRUE;
}

static void stream_plug_cb(liStream *stream, liStreamEvent event) {
	switch (event) {
	case LI_STREAM_NEW_DATA:
		if (!stream->out->is_closed && NULL != stream->source) {
			li_chunkqueue_steal_all(stream->out, stream->source->out);
			stream->out->is_closed = stream->out->is_closed || stream->source->out->is_closed;
			li_stream_notify_later(stream);
		}
		if (stream->out->is_closed) {
			li_stream_disconnect(stream);
		}
		break;
	case LI_STREAM_DISCONNECTED_DEST:
	case LI_STREAM_DISCONNECTED_SOURCE:
		li_stream_disconnect(stream);
		break;
	case LI_STREAM_DESTROY:
		g_slice_free(liStream, stream);
		break;
	default:
		break;
	}
}

liStream* li_stream_plug_new(liEventLoop *loop) {
	liStream *stream = g_slice_new0(liStream);
	li_stream_init(stream, loop, stream_plug_cb);
	return stream;
}

static void stream_null_cb(liStream *stream, liStreamEvent event) {
	switch (event) {
	case LI_STREAM_NEW_DATA:
		if (NULL == stream->source) return;
		li_chunkqueue_skip_all(stream->source->out);
		if (stream->source->out->is_closed) li_stream_disconnect(stream);
		break;
	case LI_STREAM_DESTROY:
		g_slice_free(liStream, stream);
		break;
	default:
		break;
	}
}

liStream* li_stream_null_new(liEventLoop *loop) {
	liStream *stream = g_slice_new0(liStream);
	li_stream_init(stream, loop, stream_null_cb);
	stream->out->is_closed = TRUE;
	return stream;
}


static void iostream_destroy(liIOStream *iostream) {
	int fd;

	if (0 < iostream->stream_out.refcount || 0 < iostream->stream_in.refcount) return;
	iostream->stream_out.refcount = iostream->stream_in.refcount = 1;

	if (NULL != iostream->stream_in_limit) {
		if (&iostream->io_watcher == iostream->stream_in_limit->io_watcher) {
			iostream->stream_in_limit->io_watcher = NULL;
		}
		li_cqlimit_release(iostream->stream_in_limit);
		iostream->stream_in_limit = NULL;
	}

	if (NULL != iostream->write_timeout_queue) {
		li_waitqueue_remove(iostream->write_timeout_queue, &iostream->write_timeout_elem);
		iostream->write_timeout_queue = NULL;
	}

	iostream->cb(iostream, LI_IOSTREAM_DESTROY);

	fd = li_event_io_fd(&iostream->io_watcher);
	if (-1 != fd) close(fd); /* usually this should be shutdown+closed somewhere else */
	li_event_clear(&iostream->io_watcher);

	li_iostream_throttle_clear(iostream);

	LI_FORCE_ASSERT(1 == iostream->stream_out.refcount);
	LI_FORCE_ASSERT(1 == iostream->stream_in.refcount);

	g_slice_free(liIOStream, iostream);
}

static void iostream_in_cb(liStream *stream, liStreamEvent event) {
	liIOStream *iostream = LI_CONTAINER_OF(stream, liIOStream, stream_in);

	switch (event) {
	case LI_STREAM_NEW_DATA:
		if (0 == li_chunkqueue_limit_available(stream->out)) {
			/* locked */
			return;
		}
		if (!iostream->throttled_in && iostream->can_read) {
			goffset curoutlen = stream->out->bytes_in;
			gboolean curout_closed = stream->out->is_closed;

			iostream->cb(iostream, LI_IOSTREAM_READ);

			if (curoutlen != stream->out->bytes_in || curout_closed != stream->out->is_closed) {
				li_stream_notify_later(stream);
			}

			if (-1 == li_event_io_fd(&iostream->io_watcher)) return;

			if (!iostream->throttled_in && iostream->can_read) {
				li_stream_again_later(stream);
			}
		}
		if (!iostream->throttled_in && !iostream->can_read && !iostream->in_closed) {
			li_event_io_add_events(&iostream->io_watcher, LI_EV_READ);
		}
		if (!iostream->throttled_out && !iostream->can_write && !iostream->out_closed) {
			li_event_io_add_events(&iostream->io_watcher, LI_EV_WRITE);
		}
		break;
	case LI_STREAM_NEW_CQLIMIT:
		if (NULL != iostream->stream_in_limit) {
			if (&iostream->io_watcher == iostream->stream_in_limit->io_watcher) {
				iostream->stream_in_limit->io_watcher = NULL;
			}
			li_cqlimit_release(iostream->stream_in_limit);
		}
		if (stream->out->limit) {
			stream->out->limit->io_watcher = &iostream->io_watcher;
			li_cqlimit_acquire(stream->out->limit);
		}
		iostream->stream_in_limit = stream->out->limit;
		break;
	case LI_STREAM_CONNECTED_SOURCE:
		/* there is no incoming data */
		li_stream_disconnect(stream);
		break;
	case LI_STREAM_CONNECTED_DEST:
		iostream->cb(iostream, LI_IOSTREAM_CONNECTED_DEST);
		break;
	case LI_STREAM_DISCONNECTED_DEST:
		iostream->cb(iostream, LI_IOSTREAM_DISCONNECTED_DEST);
		break;
	case LI_STREAM_DESTROY:
		if (NULL != iostream->throttle_in) {
			li_throttle_free(li_worker_from_iostream(iostream), iostream->throttle_in);
			iostream->throttle_in = NULL;
		}
		iostream->can_read = FALSE;
		iostream_destroy(iostream);
		break;
	default:
		break;
	}
}

static void iostream_out_cb(liStream *stream, liStreamEvent event) {
	liIOStream *iostream = LI_CONTAINER_OF(stream, liIOStream, stream_out);

	switch (event) {
	case LI_STREAM_NEW_DATA:
		if (!iostream->throttled_out && iostream->can_write) {
			liEventLoop *loop = li_event_get_loop(&iostream->io_watcher);
			li_tstamp now = li_event_now(loop);

			iostream->cb(iostream, LI_IOSTREAM_WRITE);
			if (NULL != iostream->write_timeout_queue) {
				if (stream->out->length > 0) {
					if (!iostream->write_timeout_elem.queued || (iostream->write_timeout_elem.ts + 1.0) < now) {
						li_waitqueue_push(iostream->write_timeout_queue, &iostream->write_timeout_elem);
					}
				} else {
					li_waitqueue_remove(iostream->write_timeout_queue, &iostream->write_timeout_elem);
				}
			}

			if (-1 == li_event_io_fd(&iostream->io_watcher)) return;

			if (iostream->can_write && !iostream->throttled_out) {
				if (stream->out->length > 0 || stream->out->is_closed) {
					li_stream_again_later(stream);
				}
			}
		}
		if (!iostream->throttled_in && !iostream->can_read && !iostream->in_closed) {
			li_event_io_add_events(&iostream->io_watcher, LI_EV_READ);
		}
		if (!iostream->throttled_out && !iostream->can_write && !iostream->out_closed) {
			li_event_io_add_events(&iostream->io_watcher, LI_EV_WRITE);
		}
		break;
	case LI_STREAM_CONNECTED_DEST:
		/* there is no outgoing data */
		li_stream_disconnect_dest(stream);
		break;
	case LI_STREAM_CONNECTED_SOURCE:
		iostream->cb(iostream, LI_IOSTREAM_CONNECTED_SOURCE);
		break;
	case LI_STREAM_DISCONNECTED_SOURCE:
		iostream->cb(iostream, LI_IOSTREAM_DISCONNECTED_SOURCE);
		break;
	case LI_STREAM_DESTROY:
		if (NULL != iostream->throttle_out) {
			li_throttle_free(li_worker_from_iostream(iostream), iostream->throttle_out);
			iostream->throttle_out = NULL;
		}
		iostream->can_write = FALSE;
		iostream_destroy(iostream);
		break;
	default:
		break;
	}
}

static void iostream_io_cb(liEventBase *watcher, int events) {
	liIOStream *iostream = LI_CONTAINER_OF(li_event_io_from(watcher), liIOStream, io_watcher);
	gboolean do_write = FALSE;

	li_event_io_rem_events(&iostream->io_watcher, LI_EV_WRITE | LI_EV_READ);

	if (0 != (events & LI_EV_WRITE) && !iostream->can_write && iostream->stream_out.refcount > 0) {
		iostream->can_write = TRUE;
		do_write = TRUE;
		li_stream_acquire(&iostream->stream_out); /* keep out stream alive during li_stream_again(&iostream->stream_in) */
	}

	if (0 != (events & LI_EV_READ) && !iostream->can_read && iostream->stream_in.refcount > 0) {
		iostream->can_read = TRUE;
		li_stream_again_later(&iostream->stream_in);
	}

	if (do_write) {
		li_stream_again_later(&iostream->stream_out);
		li_stream_release(&iostream->stream_out);
	}
}

liIOStream* li_iostream_new(liWorker *wrk, int fd, liIOStreamCB cb, gpointer data) {
	liIOStream *iostream = g_slice_new0(liIOStream);

	li_stream_init(&iostream->stream_in, &wrk->loop, iostream_in_cb);
	li_stream_init(&iostream->stream_out, &wrk->loop, iostream_out_cb);
	iostream->stream_in_limit = NULL;

	iostream->write_timeout_queue = NULL;

	li_event_io_init(&wrk->loop, "iostream", &iostream->io_watcher, iostream_io_cb, fd, LI_EV_READ);

	iostream->in_closed = iostream->out_closed = iostream->can_read = FALSE;
	iostream->can_write = TRUE;

	iostream->cb = cb;
	iostream->data = data;

	li_event_start(&iostream->io_watcher);

	return iostream;
}

void li_iostream_acquire(liIOStream* iostream) {
	li_stream_acquire(&iostream->stream_in);
	li_stream_acquire(&iostream->stream_out);
}

void li_iostream_release(liIOStream* iostream) {
	if (iostream == NULL) return;
	li_stream_release(&iostream->stream_in);
	li_stream_release(&iostream->stream_out);
}

int li_iostream_reset(liIOStream *iostream) {
	int fd;
	if (NULL == iostream) return -1;

	fd = li_event_io_fd(&iostream->io_watcher);

	li_event_clear(&iostream->io_watcher);

	if (NULL != iostream->write_timeout_queue) {
		li_waitqueue_remove(iostream->write_timeout_queue, &iostream->write_timeout_elem);
		iostream->write_timeout_queue = NULL;
	}

	li_stream_disconnect(&iostream->stream_out);
	li_stream_disconnect_dest(&iostream->stream_in);

	return fd;
}

void li_iostream_detach(liIOStream *iostream) {
	li_event_detach(&iostream->io_watcher);

	if (NULL != iostream->stream_in_limit) {
		if (&iostream->io_watcher == iostream->stream_in_limit->io_watcher) {
			iostream->stream_in_limit->io_watcher = NULL;
		}
		li_cqlimit_release(iostream->stream_in_limit);
		iostream->stream_in_limit = NULL;
	}

	li_stream_detach(&iostream->stream_in);
	li_stream_detach(&iostream->stream_out);
}

void li_iostream_attach(liIOStream *iostream, liWorker *wrk) {
	li_stream_attach(&iostream->stream_in, &wrk->loop);
	li_stream_attach(&iostream->stream_out, &wrk->loop);

	li_event_attach(&wrk->loop, &iostream->io_watcher);
}

void li_iostream_throttle_clear(liIOStream *iostream) {
	liWorker *wrk = li_worker_from_iostream(iostream);

	if (NULL != iostream->throttle_in) {
		li_throttle_free(wrk, iostream->throttle_in);
		iostream->throttle_in = NULL;
	}
	if (NULL != iostream->throttle_out) {
		li_throttle_free(wrk, iostream->throttle_out);
		iostream->throttle_out = NULL;
	}
}

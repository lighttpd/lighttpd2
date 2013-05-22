#ifndef _LIGHTTPD_STREAM_H_
#define _LIGHTTPD_STREAM_H_

#ifndef _LIGHTTPD_BASE_H_
#error Please include <lighttpd/base.h> instead of this file
#endif

#include <lighttpd/jobqueue.h>

typedef void (*liStreamCB)(liStream *stream, liStreamEvent event);

struct liStream {
	gint refcount;

	liStream *source, *dest;

	liChunkQueue *out;

	liJob new_data_job;
	liJobQueue *jobqueue;

	liStreamCB cb;
};

LI_API const gchar* li_stream_event_string(liStreamEvent event);

LI_API void li_stream_init(liStream* stream, liJobQueue *jobqueue, liStreamCB cb);
LI_API void li_stream_acquire(liStream* stream);
LI_API void li_stream_release(liStream* stream);
INLINE void li_stream_safe_release(liStream** pstream);
INLINE void li_stream_safe_reset_and_release(liStream** pstream);

LI_API void li_stream_connect(liStream *source, liStream *dest);
LI_API void li_stream_disconnect(liStream *stream); /* disconnects stream->source and stream */
LI_API void li_stream_disconnect_dest(liStream *stream); /* disconnects stream->dest and stream. only for errors/conection resets */
LI_API void li_stream_reset(liStream *stream); /* disconnect both sides */

LI_API void li_stream_notify(liStream *stream); /* new data in stream->cq, notify stream->dest */
LI_API void li_stream_notify_later(liStream *stream);
LI_API void li_stream_again(liStream *stream); /* more data to be generated in stream with event NEW_DATA or more data to be read from stream->source->cq */
LI_API void li_stream_again_later(liStream *stream);

/* detach from jobqueue, stops all event handling. you have to detach all connected streams to move streams between threads */
LI_API void li_stream_detach(liStream *stream);
LI_API void li_stream_attach(liStream *stream, liJobQueue *jobqueue); /* attach to another jobqueue - possibly after switching threads */

/* walks from first using ->dest until it reaches NULL or (it reached last and NULL != i->limit) or limit == i->cq->limit and
 * sets i->cq->limit to limit, triggering LI_STREAM_NEW_CQLIMIT.
 *   limit must not be NULL!
 */
LI_API void li_stream_set_cqlimit(liStream *first, liStream *last, liCQLimit *limit);

/* checks whether all chunkqueues in a range of streams are empty. one of first and last can be NULL to walk all stream in a direction */
LI_API gboolean li_streams_empty(liStream *first, liStream *last);

LI_API liStream* li_stream_plug_new(liJobQueue *jobqueue); /* simple forwarder; can also be used for providing data from memory */
LI_API liStream* li_stream_null_new(liJobQueue *jobqueue); /* eats everything, disconnects source on eof, out is always closed */



typedef void (*liIOStreamCB)(liIOStream *stream, liIOStreamEvent event);

/* TODO: support throttle */
struct liIOStream {
	liStream stream_in, stream_out;
	liCQLimit *stream_in_limit;

	/* initialize these before connecting stream_out if you need them */
	liWaitQueue *write_timeout_queue;
	liWaitQueueElem write_timeout_elem;

	liWorker *wrk;
	ev_io io_watcher;

	/* whether we want to read/write */
	gboolean in_closed, out_closed;
	gboolean can_read, can_write; /* set to FALSE if you got EAGAIN */

	liIOStreamCB cb;

	gpointer data; /* data for the callback */
};

LI_API const gchar* li_iostream_event_string(liIOStreamEvent event);

LI_API liIOStream* li_iostream_new(liWorker *wrk, int fd, liIOStreamCB cb, gpointer data);
LI_API void li_iostream_acquire(liIOStream* iostream);
LI_API void li_iostream_release(liIOStream* iostream);

LI_API int li_iostream_reset(liIOStream *iostream); /* returns fd, disconnects everything, stop callbacks, releases one reference */

/* similar to stream_detach/_attach */
LI_API void li_iostream_detach(liIOStream *iostream);
LI_API void li_iostream_attach(liIOStream *iostream, liWorker *wrk);

/* handles basic tcp/unix socket connections, writing and reading data, supports throttling */
LI_API void li_stream_simple_socket_close(liIOStream *stream, gboolean aborted);
LI_API void li_stream_simple_socket_io_cb(liIOStream *stream, liIOStreamEvent event);
LI_API void li_stream_simple_socket_io_cb_with_context(liIOStream *stream, liIOStreamEvent event, gpointer *data);


/* inline implementations */

INLINE void li_stream_safe_release(liStream** pstream) {
	liStream *stream;
	if (NULL == pstream || NULL == (stream = *pstream)) return;
	*pstream = NULL;
	li_stream_release(stream);
}

INLINE void li_stream_safe_reset_and_release(liStream** pstream) {
	liStream *stream;
	if (NULL == pstream || NULL == (stream = *pstream)) return;
	*pstream = NULL;
	li_stream_reset(stream);
	li_stream_release(stream);
}

#endif

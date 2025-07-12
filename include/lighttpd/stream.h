#ifndef _LIGHTTPD_STREAM_H_
#define _LIGHTTPD_STREAM_H_

#ifndef _LIGHTTPD_BASE_H_
#error Please include <lighttpd/base.h> instead of this file
#endif

#include <lighttpd/events.h>

typedef void (*liStreamCB)(liStream *stream, liStreamEvent event);

struct liStream {
	gint refcount;

	liStream *source, *dest;

	liChunkQueue *out;

	liJob new_data_job;
	liEventLoop *loop;

	liStreamCB cb;
};

LI_API const gchar* li_stream_event_string(liStreamEvent event);

LI_API void li_stream_init(liStream* stream, liEventLoop *loop, liStreamCB cb);
LI_API void li_stream_acquire(liStream* stream);
LI_API void li_stream_release(liStream* stream);
INLINE void li_stream_safe_release(liStream** pstream);
INLINE void li_stream_safe_reset_and_release(liStream** pstream);

LI_API void li_stream_connect(liStream *source, liStream *dest);
LI_API void li_stream_disconnect(liStream *stream); /* disconnects stream->source and stream */
LI_API void li_stream_disconnect_dest(liStream *stream); /* disconnects stream->dest and stream. only for errors/connection resets */
LI_API void li_stream_reset(liStream *stream); /* disconnect both sides */

LI_API void li_stream_notify(liStream *stream); /* new data in stream->cq, notify stream->dest */
LI_API void li_stream_notify_later(liStream *stream);
LI_API void li_stream_again(liStream *stream); /* more data to be generated in stream with event NEW_DATA or more data to be read from stream->source->cq */
LI_API void li_stream_again_later(liStream *stream);

/* detach from jobqueue, stops all event handling. you have to detach all connected streams to move streams between threads */
LI_API void li_stream_detach(liStream *stream);
LI_API void li_stream_attach(liStream *stream, liEventLoop *loop); /* attach to another loop - possibly after switching threads */

/* walks from first using ->dest until it reaches NULL or (it reached last and NULL != i->limit) or limit == i->cq->limit and
 * sets i->cq->limit to limit, triggering LI_STREAM_NEW_CQLIMIT.
 *   limit must not be NULL!
 */
LI_API void li_stream_set_cqlimit(liStream *first, liStream *last, liCQLimit *limit);

/* checks whether all chunkqueues in a range of streams are empty. one of first and last can be NULL to walk all stream in a direction */
LI_API gboolean li_streams_empty(liStream *first, liStream *last);

LI_API liStream* li_stream_plug_new(liEventLoop *loop); /* simple forwarder; can also be used for providing data from memory */
LI_API liStream* li_stream_null_new(liEventLoop *loop); /* eats everything, disconnects source on eof, out is always closed */

typedef void (*liIOStreamCB)(liIOStream *stream, liIOStreamEvent event);

struct liIOStream {
	liStream stream_in, stream_out;
	liCQLimit *stream_in_limit;

	/* initialize these before connecting stream_out if you need them */
	liWaitQueue *write_timeout_queue;
	liWaitQueueElem write_timeout_elem;

	liEventIO io_watcher;

	/* whether we want to read/write */
	guint in_closed:1, out_closed:1;
	guint can_read:1, can_write:1; /* set to FALSE if you got EAGAIN */
	guint throttled_in:1, throttled_out:1;

	/* throttle needs to be handled by the liIOStreamCB cb */
	liThrottleState *throttle_in;
	liThrottleState *throttle_out;

	liIOStreamCB cb;

	gpointer data; /* data for the callback */
};

LI_API const gchar* li_iostream_event_string(liIOStreamEvent event);

LI_API liIOStream* li_iostream_new(liWorker *wrk, int fd, liIOStreamCB cb, gpointer data);
LI_API void li_iostream_acquire(liIOStream* iostream);
LI_API void li_iostream_release(liIOStream* iostream);
INLINE void li_iostream_safe_release(liIOStream** piostream);

LI_API int li_iostream_reset(liIOStream *iostream); /* returns fd, disconnects everything, stop callbacks, releases one reference */

/* unset throttle_out and throttle_in */
LI_API void li_iostream_throttle_clear(liIOStream *iostream);

/* similar to stream_detach/_attach */
LI_API void li_iostream_detach(liIOStream *iostream);
LI_API void li_iostream_attach(liIOStream *iostream, liWorker *wrk);

/* handles basic tcp/unix socket connections, writing and reading data, supports throttling */
LI_API void li_stream_simple_socket_close(liIOStream *stream, gboolean aborted);
/* li_stream_simple_socket_io_cb can be used with li_iostream_new and uses stream->data as buffer */
LI_API void li_stream_simple_socket_io_cb(liIOStream *stream, liIOStreamEvent event);
/* read buffer probably should be (indirectly) stored in stream->data; LI_IOSTREAM_DESTROY frees the buffer and clears the pointer */
LI_API void li_stream_simple_socket_io_cb_with_buffer(liIOStream *stream, liIOStreamEvent event, liBuffer **buffer);
/* tries to flush TCP sockets by disabling nagle */
LI_API void li_stream_simple_socket_flush(liIOStream *stream);


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

INLINE void li_iostream_safe_release(liIOStream** piostream) {
	liIOStream *iostream;
	if (NULL == piostream || NULL == (iostream = *piostream)) return;
	*piostream = NULL;
	li_iostream_release(iostream);
}

#endif


#include <lighttpd/base.h>
#include <lighttpd/throttle.h>
#include <lighttpd/plugin_core.h>

#define LI_CONNECTION_DEFAULT_CHUNKQUEUE_LIMIT (256*1024)

void li_connection_simple_tcp(liConnection **pcon, liIOStream *stream, gpointer *context, liIOStreamEvent event) {
	liConnection *con;
	goffset transfer_in = 0, transfer_out = 0;

	transfer_in = (NULL != stream->stream_in.out) ? stream->stream_in.out->bytes_in : 0;
	transfer_out = (NULL != stream->stream_out.out) ? stream->stream_out.out->bytes_out : 0;

	li_stream_simple_socket_io_cb_with_context(stream, event, context);

	/* li_stream_simple_socket_io_cb_with_context might lead to *pcon == NULL */
	con = *pcon;
	if (NULL != con) {
		if (NULL != stream->stream_in.out) {
			transfer_in = stream->stream_in.out->bytes_in - transfer_in;
			if (transfer_in > 0) {
				li_connection_update_io_timeout(con);
				li_vrequest_update_stats_in(con->mainvr, transfer_in);
			}
		}
		if (NULL != stream->stream_out.out) {
			transfer_out = stream->stream_out.out->bytes_out - transfer_out;
			if (transfer_out > 0) {
				li_connection_update_io_timeout(con);
				li_vrequest_update_stats_out(con->mainvr, transfer_out);
			}
		}
	}

	switch (event) {
	case LI_IOSTREAM_DESTROY:
		li_stream_simple_socket_close(stream, FALSE);
		return;
	case LI_IOSTREAM_DISCONNECTED_DEST:
		if (NULL != stream->stream_in.out && !stream->stream_in.out->is_closed) {
			li_stream_simple_socket_close(stream, TRUE);
			return;
		}
		break;
	case LI_IOSTREAM_DISCONNECTED_SOURCE:
		if (NULL != stream->stream_out.out && !stream->stream_out.out->is_closed) {
			li_stream_simple_socket_close(stream, TRUE);
			return;
		}
		break;
	default:
		break;
	}

	if ((NULL == stream->stream_in.out || stream->stream_in.out->is_closed) &&
		!(NULL == stream->stream_out.out || stream->stream_out.out->is_closed)) {
		stream->stream_out.out->is_closed = TRUE;
		li_stream_again_later(&stream->stream_out);
	}
}


typedef struct simple_tcp_connection simple_tcp_connection;
struct simple_tcp_connection {
	liIOStream *sock_stream;
	gpointer simple_tcp_context;
	liConnection *con;
};

static void simple_tcp_io_cb(liIOStream *stream, liIOStreamEvent event) {
	simple_tcp_connection *data = stream->data;
	LI_FORCE_ASSERT(NULL != data);
	LI_FORCE_ASSERT(NULL == data->con || data == data->con->con_sock.data);
	LI_FORCE_ASSERT(NULL == data->sock_stream || stream == data->sock_stream);

	li_connection_simple_tcp(&data->con, stream, &data->simple_tcp_context, event);

	if (NULL != data->con && data->con->out_has_all_data
	    && (NULL == stream->stream_out.out || 0 == stream->stream_out.out->length)) {
		li_stream_simple_socket_flush(stream);
		li_connection_request_done(data->con);
	}

	switch (event) {
	case LI_IOSTREAM_DESTROY:
		LI_FORCE_ASSERT(NULL == data->con);
		LI_FORCE_ASSERT(NULL == data->sock_stream);
		stream->data = NULL;
		g_slice_free(simple_tcp_connection, data);
		break;
	default:
		break;
	}
}

static void simple_tcp_finished(liConnection *con, gboolean aborted) {
	simple_tcp_connection *data = con->con_sock.data;
	liIOStream *stream;
	if (NULL == data) return;

	data->con = NULL;
	con->con_sock.data = NULL;
	con->con_sock.callbacks = NULL;

	stream = data->sock_stream;
	data->sock_stream = NULL;

	li_stream_simple_socket_close(stream, aborted);
	li_iostream_release(stream);

	{
		liStream *raw_out = con->con_sock.raw_out, *raw_in = con->con_sock.raw_in;
		con->con_sock.raw_out = con->con_sock.raw_in = NULL;
		if (NULL != raw_out) { li_stream_reset(raw_out); li_stream_release(raw_out); }
		if (NULL != raw_in) { li_stream_reset(raw_in); li_stream_release(raw_in); }
	}
}

static liThrottleState* simple_tcp_throttle_out(liConnection *con) {
	simple_tcp_connection *data = con->con_sock.data;
	if (NULL == data) return NULL;
	if (NULL == data->sock_stream->throttle_out) data->sock_stream->throttle_out = li_throttle_new();
	return data->sock_stream->throttle_out;
}

static liThrottleState* simple_tcp_throttle_in(liConnection *con) {
	simple_tcp_connection *data = con->con_sock.data;
	if (NULL == data) return NULL;
	if (NULL == data->sock_stream->throttle_in) data->sock_stream->throttle_in = li_throttle_new();
	return data->sock_stream->throttle_in;
}

static const liConnectionSocketCallbacks simple_tcp_cbs = {
	simple_tcp_finished,
	simple_tcp_throttle_out,
	simple_tcp_throttle_in
};

static gboolean simple_tcp_new(liConnection *con, int fd) {
	simple_tcp_connection *data = g_slice_new0(simple_tcp_connection);
	data->sock_stream = li_iostream_new(con->wrk, fd, simple_tcp_io_cb, data);
	data->simple_tcp_context = NULL;
	data->con = con;
	con->con_sock.data = data;
	con->con_sock.callbacks = &simple_tcp_cbs;
	con->con_sock.raw_out = &data->sock_stream->stream_out;
	con->con_sock.raw_in = &data->sock_stream->stream_in;
	li_stream_acquire(con->con_sock.raw_out);
	li_stream_acquire(con->con_sock.raw_in);

	return TRUE;
}






static void con_iostream_close(liConnection *con) { /* force close */
	if (con->con_sock.callbacks) {
		con->info.aborted = TRUE;
		con->con_sock.callbacks->finish(con, TRUE);
	}
	LI_FORCE_ASSERT(NULL == con->con_sock.data);
}
static void con_iostream_shutdown(liConnection *con) { /* (try) regular shutdown */
	if (NULL != con->con_sock.raw_out) {
		con->con_sock.raw_out->out->is_closed = TRUE;
		li_stream_notify(con->con_sock.raw_out);
	}

	if (con->con_sock.callbacks) {
		con->con_sock.callbacks->finish(con, FALSE);
	}
	LI_FORCE_ASSERT(NULL == con->con_sock.data);
}


static void connection_close(liConnection *con);
static void li_connection_reset_keep_alive(liConnection *con);

static void li_connection_reset2(liConnection *con); /* reset when dead and stream refs down */

static void connection_check_reset(liJob *job) {
	liConnection *con = LI_CONTAINER_OF(job, liConnection, job_reset);

	if (LI_CON_STATE_DEAD == con->state && (0 == con->in.refcount) && (0 == con->out.refcount)) {
		li_connection_reset2(con);
		li_worker_con_put(con);
	}
}

/* tcp/ssl -> http "parser" */
static void _connection_http_in_cb(liStream *stream, liStreamEvent event) {
	liConnection *con = LI_CONTAINER_OF(stream, liConnection, in);
	liChunkQueue *raw_in, *in;
	liVRequest *vr = con->mainvr;

	switch (event) {
	case LI_STREAM_NEW_DATA:
		/* handle below */
		break;
	case LI_STREAM_DISCONNECTED_SOURCE:
		connection_close(con);
		return;
	case LI_STREAM_DESTROY:
		con->info.req = NULL;
		li_job_later(&con->wrk->loop.jobqueue, &con->job_reset);
		return;
	default:
		return;
	}

	if (NULL == stream->source) return;

	/* raw_in never gets closed normally - if we receive EOF from the client it means it cancelled the request */
	raw_in = stream->source->out;
	if (raw_in->is_closed) {
		connection_close(con);
		return;
	}

	/* always close "in" after request body end. reopen it on keep-alive */
	in = con->in.out;

	if (0 == raw_in->length) return; /* no (new) data */

	if (LI_CON_STATE_UPGRADED == con->state) {
		li_chunkqueue_steal_all(in, raw_in);
		li_stream_notify(stream);
		return;
	}

	if (con->state == LI_CON_STATE_KEEP_ALIVE) {
		/* stop keep alive timeout watchers */
		if (con->keep_alive_data.link) {
			g_queue_delete_link(&con->wrk->keep_alive_queue, con->keep_alive_data.link);
			con->keep_alive_data.link = NULL;
		}
		con->keep_alive_data.timeout = 0;
		li_event_stop(&con->keep_alive_data.watcher);

		con->keep_alive_requests++;
		/* disable keep alive if limit is reached */
		if (con->keep_alive_requests == CORE_OPTION(LI_CORE_OPTION_MAX_KEEP_ALIVE_REQUESTS).number)
			con->info.keep_alive = FALSE;

		/* reopen stream for request body */
		li_chunkqueue_reset(in);
		/* reset stuff from keep-alive and record timestamp */
		li_vrequest_start(con->mainvr);

		con->state = LI_CON_STATE_READ_REQUEST_HEADER;

		/* put back in io timeout queue */
		li_connection_update_io_wait(con);
	} else if (con->state == LI_CON_STATE_REQUEST_START) {
		con->state = LI_CON_STATE_READ_REQUEST_HEADER;
		li_connection_update_io_wait(con);
	}

	if (con->state == LI_CON_STATE_READ_REQUEST_HEADER) {
		liHandlerResult res;

		if (CORE_OPTION(LI_CORE_OPTION_DEBUG_REQUEST_HANDLING).boolean) {
			VR_DEBUG(vr, "%s", "reading request header");
		}

		res = li_http_request_parse(vr, &con->req_parser_ctx);

		/* max uri length 8 kilobytes */
		/* TODO: check this and similar in request_parse and response_parse */
		if (vr->request.uri.raw->len > 8*1024) {
			VR_INFO(vr,
				"request uri too large. limit: 8kb, received: %s",
				li_counter_format(vr->request.uri.raw->len, COUNTER_BYTES, vr->wrk->tmp_str)->str
			);

			con->info.keep_alive = FALSE;
			vr->response.http_status = 414; /* Request-URI Too Large */
			con->state = LI_CON_STATE_WRITE;
			li_connection_update_io_wait(con);
			li_stream_again(&con->out);
			return;
		}

		switch(res) {
		case LI_HANDLER_GO_ON:
			break; /* go on */
		case LI_HANDLER_WAIT_FOR_EVENT:
			return;
		case LI_HANDLER_ERROR:
		case LI_HANDLER_COMEBACK: /* unexpected */
			/* unparsable header */
			if (CORE_OPTION(LI_CORE_OPTION_DEBUG_REQUEST_HANDLING).boolean) {
				VR_DEBUG(vr, "%s", "parsing header failed");
			}

			con->wrk->stats.requests++;
			con->info.keep_alive = FALSE;
			/* set status 400 if not already set to e.g. 413 */
			if (vr->response.http_status == 0)
				vr->response.http_status = 400;
			con->state = LI_CON_STATE_WRITE;
			li_connection_update_io_wait(con);
			li_stream_again(&con->out);
			return;
		}

		con->wrk->stats.requests++;

		/* headers ready */
		if (CORE_OPTION(LI_CORE_OPTION_DEBUG_REQUEST_HANDLING).boolean) {
			VR_DEBUG(vr, "%s", "validating request header");
		}
		if (!li_request_validate_header(con)) {
			/* set status 400 if not already set */
			if (vr->response.http_status == 0)
				vr->response.http_status = 400;
			con->state = LI_CON_STATE_WRITE;
			con->info.keep_alive = FALSE;
			li_connection_update_io_wait(con);
			li_stream_again(&con->out);
			return;
		}

		/* When does a client ask for 100 Continue? probably not while trying to ddos us
		 * as post content probably goes to a dynamic backend anyway, we don't
		 * care about the rare cases we could determine that we don't want a request at all
		 * before sending it to a backend - so just send the stupid header
		 */
		if (con->expect_100_cont) {
			if (CORE_OPTION(LI_CORE_OPTION_DEBUG_REQUEST_HANDLING).boolean) {
				VR_DEBUG(vr, "%s", "send 100 Continue");
			}
			li_chunkqueue_append_mem(con->out.out, CONST_STR_LEN("HTTP/1.1 100 Continue\r\n\r\n"));
			con->expect_100_cont = FALSE;

			li_stream_notify(&con->out);
		}

		con->state = LI_CON_STATE_HANDLE_MAINVR;
		li_connection_update_io_wait(con);
		li_action_enter(vr, con->srv->mainaction);

		li_vrequest_handle_request_headers(vr);
	}

	if (con->state != LI_CON_STATE_READ_REQUEST_HEADER && !in->is_closed) {
		goffset newbytes = 0;

		if (-1 == vr->request.content_length) {
			if (!in->is_closed) {
				if (!li_filter_chunked_decode(vr, in, raw_in, &con->in_chunked_decode_state)) {
					if (CORE_OPTION(LI_CORE_OPTION_DEBUG_REQUEST_HANDLING).boolean) {
						VR_DEBUG(vr, "%s", "failed decoding chunked request body");
					}
					li_connection_error(con);
					return;
				}
				if (in->is_closed) vr->request.content_length = in->bytes_in;
				newbytes = 1; /* always notify */
			}
		} else {
			if (in->bytes_in < vr->request.content_length) {
				newbytes = li_chunkqueue_steal_len(in, raw_in, vr->request.content_length - in->bytes_in);
			}
			if (in->bytes_in == vr->request.content_length) {
				in->is_closed = TRUE;
			}
		}
		if (newbytes > 0 || in->is_closed) {
			li_stream_notify(&con->in);
		}
	}
}

/* http response header/data -> tcp/ssl */
static void _connection_http_out_cb(liStream *stream, liStreamEvent event) {
	liConnection *con = LI_CONTAINER_OF(stream, liConnection, out);

	liChunkQueue *raw_out = stream->out, *out;
	liVRequest *vr = con->mainvr;

	switch (event) {
	case LI_STREAM_NEW_DATA:
		/* handle below */
		break;
	case LI_STREAM_CONNECTED_SOURCE:
		/* also handle data immediately */
		break;
	case LI_STREAM_DISCONNECTED_SOURCE:
		if (!con->out_has_all_data) li_connection_error(con);
		return;
	case LI_STREAM_DISCONNECTED_DEST:
		if (!raw_out->is_closed || 0 != raw_out->length || NULL == con->con_sock.raw_out) {
			li_connection_error(con);
		} else {
			connection_close(con);
		}
		return;
	case LI_STREAM_DESTROY:
		con->info.resp = NULL;
		li_job_later(&con->wrk->loop.jobqueue, &con->job_reset);
		return;
	default:
		return;
	}

	out = (NULL != stream->source) ? stream->source->out : NULL;

	/* keep raw_out->is_closed = FALSE for keep-alive requests; instead set con->out_has_all_data = TRUE */

	if (LI_CON_STATE_HANDLE_MAINVR <= con->state) {
		if (NULL == stream->source) {
			if (LI_CON_STATE_HANDLE_MAINVR == con->state) {
				/* wait for vrequest to connect the stream as signal that the headers are ready */
				return;
			}
		}
		if (!con->response_headers_sent) {
			if (CORE_OPTION(LI_CORE_OPTION_DEBUG_REQUEST_HANDLING).boolean) {
				VR_DEBUG(vr, "%s", "write response headers");
			}
			con->response_headers_sent = TRUE;
			if (0 == CORE_OPTION(LI_CORE_OPTION_MAX_KEEP_ALIVE_IDLE).number) {
				con->info.keep_alive = FALSE;
			}
			li_response_send_headers(vr, raw_out, out, FALSE);
		}

		if (!con->out_has_all_data && !raw_out->is_closed && NULL != out) {
			if (vr->response.transfer_encoding & LI_HTTP_TRANSFER_ENCODING_CHUNKED) {
				li_filter_chunked_encode(vr, raw_out, out);
			} else {
				li_chunkqueue_steal_all(raw_out, out);
			}
		}
		if (raw_out->is_closed || NULL == out || out->is_closed) {
			con->out_has_all_data = TRUE;
			raw_out->is_closed = FALSE;
		}
		if (con->out_has_all_data) {
			if (con->state < LI_CON_STATE_WRITE) {
				con->state = LI_CON_STATE_WRITE;
				li_connection_update_io_wait(con);
			}
			if (NULL != out) {
				out = NULL;
				li_stream_disconnect(stream);
			}
		}
		con->info.out_queue_length = raw_out->length;
	}

	li_stream_notify(stream);
}


void li_connection_update_io_timeout(liConnection *con) {
	liWorker *wrk = con->wrk;

	if (con->io_timeout_elem.queued && (con->io_timeout_elem.ts + 1.0) < li_cur_ts(wrk)) {
		li_waitqueue_push(&wrk->io_timeout_queue, &con->io_timeout_elem);
	}
}

void li_connection_update_io_wait(liConnection *con) {
	liWorker *wrk = con->wrk;
	gboolean want_timeout = FALSE;
	gboolean stopping = wrk->wait_for_stop_connections.active;

	switch (con->state) {
	case LI_CON_STATE_DEAD:
	case LI_CON_STATE_CLOSE: /* only a temporary state before DEAD */
		want_timeout = FALSE;
		break;
	case LI_CON_STATE_KEEP_ALIVE:
		want_timeout = stopping;
		break;
	case LI_CON_STATE_REQUEST_START:
		want_timeout = TRUE;
		break;
	case LI_CON_STATE_READ_REQUEST_HEADER:
		want_timeout = TRUE;
		break;
	case LI_CON_STATE_HANDLE_MAINVR:
		/* want timeout while we're still reading request body */
		want_timeout = stopping || !con->in.out->is_closed;
		break;
	case LI_CON_STATE_WRITE:
		want_timeout = TRUE;
		break;
	case LI_CON_STATE_UPGRADED:
		want_timeout = stopping;
		break;
	}

	if (want_timeout == con->io_timeout_elem.queued) return;
	if (want_timeout) {
		li_waitqueue_push(&wrk->io_timeout_queue, &con->io_timeout_elem);
	} else {
		li_waitqueue_remove(&wrk->io_timeout_queue, &con->io_timeout_elem);
	}
}


void li_connection_start(liConnection *con, liSocketAddress remote_addr, int s, liServerSocket *srv_sock) {
	LI_FORCE_ASSERT(NULL == con->con_sock.data);

	con->srv_sock = srv_sock;
	con->state = LI_CON_STATE_REQUEST_START;
	con->mainvr->ts_started = con->ts_started = li_cur_ts(con->wrk);

	con->info.remote_addr = remote_addr;
	li_sockaddr_to_string(remote_addr, con->info.remote_addr_str, FALSE);

	con->info.local_addr = li_sockaddr_dup(srv_sock->local_addr);
	li_sockaddr_to_string(con->info.local_addr, con->info.local_addr_str, FALSE);

	con->info.aborted = FALSE;

	li_stream_init(&con->in, &con->wrk->loop, _connection_http_in_cb);
	li_stream_init(&con->out, &con->wrk->loop, _connection_http_out_cb);

	con->info.req = &con->in;
	con->info.resp = &con->out;

	li_connection_update_io_wait(con);

	if (srv_sock->new_cb) {
		if (!srv_sock->new_cb(con, s)) {
			li_connection_error(con);
			return;
		}
	} else {
		simple_tcp_new(con, s);
	}

	LI_FORCE_ASSERT(NULL != con->con_sock.raw_in || NULL != con->con_sock.raw_out);

	li_chunkqueue_use_limit(con->con_sock.raw_in->out, LI_CONNECTION_DEFAULT_CHUNKQUEUE_LIMIT);
	li_chunkqueue_use_limit(con->con_sock.raw_out->out, LI_CONNECTION_DEFAULT_CHUNKQUEUE_LIMIT);

	li_stream_connect(&con->out, con->con_sock.raw_out);
	li_stream_connect(con->con_sock.raw_in, &con->in);

	li_chunk_parser_init(&con->req_parser_ctx.chunk_ctx, con->con_sock.raw_in->out);
}

void li_connection_request_done(liConnection *con) {
	liVRequest *vr = con->mainvr;
	liServerState s;

	if (LI_CON_STATE_CLOSE == con->state || LI_CON_STATE_DEAD == con->state) return;

	if (CORE_OPTION(LI_CORE_OPTION_DEBUG_REQUEST_HANDLING).boolean) {
		VR_DEBUG(con->mainvr, "response end (keep_alive = %i)", con->info.keep_alive);
	}

	li_plugins_handle_close(con);

	s = g_atomic_int_get(&con->srv->dest_state);
	if (con->info.keep_alive &&  (LI_SERVER_RUNNING == s || LI_SERVER_WARMUP == s) && NULL != con->con_sock.data) {
		li_connection_reset_keep_alive(con);
	} else {
		con->state = LI_CON_STATE_CLOSE;
		con_iostream_shutdown(con);
		li_connection_reset(con);
	}
}

/* in stream <-> socket disconnect event */
static void connection_close(liConnection *con) {
	liVRequest *vr = con->mainvr;

	if (LI_CON_STATE_CLOSE == con->state || LI_CON_STATE_DEAD == con->state) return;

	if (CORE_OPTION(LI_CORE_OPTION_DEBUG_REQUEST_HANDLING).boolean) {
		VR_DEBUG(vr, "%s", "connection closed");
	}

	con->state = LI_CON_STATE_CLOSE;

	con_iostream_close(con);

	li_plugins_handle_close(con);

	li_connection_reset(con);
}

void li_connection_error(liConnection *con) {
	liVRequest *vr = con->mainvr;

	if (LI_CON_STATE_CLOSE == con->state || LI_CON_STATE_DEAD == con->state) return;

	if (CORE_OPTION(LI_CORE_OPTION_DEBUG_REQUEST_HANDLING).boolean) {
		VR_DEBUG(vr, "%s", "connection closed (error)");
	}

	con->state = LI_CON_STATE_CLOSE;

	con_iostream_close(con);

	li_plugins_handle_close(con);

	li_connection_reset(con);
}

static void connection_keepalive_cb(liEventBase *watcher, int events) {
	liConnection *con = LI_CONTAINER_OF(li_event_timer_from(watcher), liConnection, keep_alive_data.watcher);
	UNUSED(events);

	li_connection_reset(con);
}

static void mainvr_handle_response_error(liVRequest *vr) {
	liConnection* con = li_connection_from_vrequest(vr);
	LI_FORCE_ASSERT(NULL != con);

	li_connection_error(con);
}

static liThrottleState* mainvr_throttle_out(liVRequest *vr) {
	liConnection* con = li_connection_from_vrequest(vr);
	LI_FORCE_ASSERT(NULL != con);

	return con->con_sock.callbacks->throttle_out(con);
}

static liThrottleState* mainvr_throttle_in(liVRequest *vr) {
	liConnection* con = li_connection_from_vrequest(vr);
	LI_FORCE_ASSERT(NULL != con);

	return con->con_sock.callbacks->throttle_in(con);
}

static void mainvr_connection_upgrade(liVRequest *vr, liStream *backend_drain, liStream *backend_source) {
	liConnection* con = li_connection_from_vrequest(vr);
	LI_FORCE_ASSERT(NULL != con);

	if (con->response_headers_sent || NULL != con->out.source) {
		li_connection_error(con);
		return;
	}
	if (CORE_OPTION(LI_CORE_OPTION_DEBUG_REQUEST_HANDLING).boolean) {
		VR_DEBUG(vr, "%s", "connection upgrade: write response headers");
	}
	con->response_headers_sent = TRUE;
	con->info.keep_alive = FALSE;
	li_response_send_headers(vr, con->out.out, NULL, TRUE);
	con->state = LI_CON_STATE_UPGRADED;
	vr->response.transfer_encoding = 0;
	li_connection_update_io_wait(con);

	li_stream_disconnect_dest(&con->in);
	con->in.out->is_closed = FALSE;

	li_stream_connect(&con->in, backend_drain);
	li_stream_connect(backend_source, &con->out);

	li_vrequest_reset(con->mainvr, TRUE);

	if (NULL != con->in.source) {
		li_chunkqueue_steal_all(con->out.out, backend_drain->out);
	}
	con->info.out_queue_length = con->out.out->length;

	li_stream_notify(&con->out);
	li_stream_notify(&con->in);
}

static const liConCallbacks con_callbacks = {
	mainvr_handle_response_error,
	mainvr_throttle_out,
	mainvr_throttle_in,
	mainvr_connection_upgrade
};

liConnection* li_connection_new(liWorker *wrk) {
	liServer *srv = wrk->srv;
	liConnection *con = g_slice_new0(liConnection);
	con->wrk = wrk;
	con->srv = srv;

	con->state = LI_CON_STATE_DEAD;
	con->response_headers_sent = FALSE;
	con->expect_100_cont = FALSE;
	con->out_has_all_data = FALSE;

	con->info.remote_addr_str = g_string_sized_new(INET6_ADDRSTRLEN);
	con->info.local_addr_str = g_string_sized_new(INET6_ADDRSTRLEN);
	con->info.is_ssl = FALSE;
	con->info.keep_alive = TRUE;

	con->info.req = NULL;
	con->info.resp = NULL;

	con->info.callbacks = &con_callbacks;

	con->mainvr = li_vrequest_new(wrk, &con->info);

	li_http_request_parser_init(&con->req_parser_ctx, &con->mainvr->request, NULL); /* chunkqueue is created in _start */

	con->keep_alive_data.link = NULL;
	con->keep_alive_data.timeout = 0;
	con->keep_alive_data.max_idle = 0;
	li_event_timer_init(&wrk->loop, &con->keep_alive_data.watcher, connection_keepalive_cb);

	con->io_timeout_elem.data = con;

	li_job_init(&con->job_reset, connection_check_reset);

	return con;
}

void li_connection_reset(liConnection *con) {
	if (LI_CON_STATE_DEAD != con->state) {
		con->state = LI_CON_STATE_DEAD;

		con_iostream_close(con);
		li_stream_reset(&con->in);
		li_stream_reset(&con->out);

		li_vrequest_reset(con->mainvr, TRUE);
		li_stream_release(&con->in);
		li_stream_release(&con->out);

		con->info.keep_alive = TRUE;
		if (con->keep_alive_data.link) {
			g_queue_delete_link(&con->wrk->keep_alive_queue, con->keep_alive_data.link);
			con->keep_alive_data.link = NULL;
		}
		con->keep_alive_data.timeout = 0;
		con->keep_alive_data.max_idle = 0;
		li_event_stop(&con->keep_alive_data.watcher);
		con->keep_alive_requests = 0;
	}

	li_connection_update_io_wait(con);
	li_job_later(&con->wrk->loop.jobqueue, &con->job_reset);
}

static void li_connection_reset2(liConnection *con) {
	con->response_headers_sent = FALSE;
	con->expect_100_cont = FALSE;
	con->out_has_all_data = FALSE;

	con_iostream_close(con);

	li_server_socket_release(con->srv_sock);
	con->srv_sock = NULL;
	con->info.is_ssl = FALSE;
	con->info.aborted = FALSE;
	con->info.out_queue_length = 0;

	li_stream_reset(&con->in);
	li_stream_reset(&con->out);

	li_vrequest_reset(con->mainvr, FALSE);

	li_http_request_parser_reset(&con->req_parser_ctx);

	g_string_truncate(con->info.remote_addr_str, 0);
	li_sockaddr_clear(&con->info.remote_addr);
	g_string_truncate(con->info.local_addr_str, 0);
	li_sockaddr_clear(&con->info.local_addr);

	con->info.keep_alive = TRUE;
	if (con->keep_alive_data.link) {
		g_queue_delete_link(&con->wrk->keep_alive_queue, con->keep_alive_data.link);
		con->keep_alive_data.link = NULL;
	}
	con->keep_alive_data.timeout = 0;
	con->keep_alive_data.max_idle = 0;
	li_event_stop(&con->keep_alive_data.watcher);
	con->keep_alive_requests = 0;

	/* reset stats */
	con->info.stats.bytes_in = G_GUINT64_CONSTANT(0);
	con->info.stats.bytes_in_5s = G_GUINT64_CONSTANT(0);
	con->info.stats.bytes_in_5s_diff = G_GUINT64_CONSTANT(0);
	con->info.stats.bytes_out = G_GUINT64_CONSTANT(0);
	con->info.stats.bytes_out_5s = G_GUINT64_CONSTANT(0);
	con->info.stats.bytes_out_5s_diff = G_GUINT64_CONSTANT(0);
	con->info.stats.last_avg = 0;

	/* remove from timeout queue */
	li_waitqueue_remove(&con->wrk->io_timeout_queue, &con->io_timeout_elem);

	li_job_reset(&con->job_reset);
}

static void li_connection_reset_keep_alive(liConnection *con) {
	liVRequest *vr = con->mainvr;

	if (NULL == con->con_sock.raw_in || NULL == con->con_sock.raw_out || con->in.source != con->con_sock.raw_in) {
		li_connection_reset(con);
		return;
	}

	/* only start keep alive watcher if there isn't more input data already */
	if (con->con_sock.raw_in->out->length == 0) {
		li_event_stop(&con->keep_alive_data.watcher);
		{
			con->keep_alive_data.max_idle = CORE_OPTION(LI_CORE_OPTION_MAX_KEEP_ALIVE_IDLE).number;
			if (con->keep_alive_data.max_idle == 0) {
				con->state = LI_CON_STATE_CLOSE;
				con_iostream_shutdown(con);
				li_connection_reset(con);
				return;
			}

			con->keep_alive_data.timeout = li_cur_ts(con->wrk) + con->keep_alive_data.max_idle;

			if (con->keep_alive_data.max_idle == con->srv->keep_alive_queue_timeout) {
				/* queue is sorted by con->keep_alive_data.timeout */
				gboolean need_start = (0 == con->wrk->keep_alive_queue.length);
				con->keep_alive_data.timeout = li_cur_ts(con->wrk) + con->srv->keep_alive_queue_timeout;
				g_queue_push_tail(&con->wrk->keep_alive_queue, con);
				con->keep_alive_data.link = g_queue_peek_tail_link(&con->wrk->keep_alive_queue);
				if (need_start)
					li_worker_check_keepalive(con->wrk);
			} else {
				li_event_timer_once(&con->keep_alive_data.watcher, con->keep_alive_data.max_idle);
			}
		}
	} else {
		li_stream_again_later(&con->in);
	}

	con->state = LI_CON_STATE_KEEP_ALIVE;
	con->response_headers_sent = FALSE;
	con->expect_100_cont = FALSE;
	con->out_has_all_data = FALSE;

	con->info.keep_alive = TRUE;

	li_connection_update_io_wait(con);

	li_vrequest_reset(con->mainvr, TRUE);
	li_http_request_parser_reset(&con->req_parser_ctx);

	li_stream_disconnect(&con->out);
	li_stream_disconnect_dest(&con->in);
	con->out.out->is_closed = FALSE;

	memset(&con->in_chunked_decode_state, 0, sizeof(con->in_chunked_decode_state));

	/* restore chunkqueue limits */
	li_chunkqueue_use_limit(con->con_sock.raw_in->out, LI_CONNECTION_DEFAULT_CHUNKQUEUE_LIMIT);
	li_chunkqueue_use_limit(con->con_sock.raw_out->out, LI_CONNECTION_DEFAULT_CHUNKQUEUE_LIMIT);

	/* reset stats */
	con->info.stats.bytes_in = G_GUINT64_CONSTANT(0);
	con->info.stats.bytes_in_5s = G_GUINT64_CONSTANT(0);
	con->info.stats.bytes_in_5s_diff = G_GUINT64_CONSTANT(0);
	con->info.stats.bytes_out = G_GUINT64_CONSTANT(0);
	con->info.stats.bytes_out_5s = G_GUINT64_CONSTANT(0);
	con->info.stats.bytes_out_5s_diff = G_GUINT64_CONSTANT(0);
	con->info.stats.last_avg = 0;
}

void li_connection_free(liConnection *con) {
	LI_FORCE_ASSERT(NULL == con->con_sock.data);
	LI_FORCE_ASSERT(LI_CON_STATE_DEAD == con->state);

	con->response_headers_sent = FALSE;
	con->expect_100_cont = FALSE;
	con->out_has_all_data = FALSE;

	li_server_socket_release(con->srv_sock);
	con->srv_sock = NULL;

	g_string_free(con->info.remote_addr_str, TRUE);
	li_sockaddr_clear(&con->info.remote_addr);
	g_string_free(con->info.local_addr_str, TRUE);
	li_sockaddr_clear(&con->info.local_addr);

	li_vrequest_free(con->mainvr);
	li_http_request_parser_clear(&con->req_parser_ctx);

	con->info.keep_alive = TRUE;
	if (con->keep_alive_data.link && con->wrk) {
		g_queue_delete_link(&con->wrk->keep_alive_queue, con->keep_alive_data.link);
		con->keep_alive_data.link = NULL;
	}
	con->keep_alive_data.timeout = 0;
	con->keep_alive_data.max_idle = 0;
	li_event_clear(&con->keep_alive_data.watcher);

	/* remove from timeout queue */
	li_waitqueue_remove(&con->wrk->io_timeout_queue, &con->io_timeout_elem);

	li_job_clear(&con->job_reset);

	g_slice_free(liConnection, con);
}

gchar *li_connection_state_str(liConnectionState state) {
	switch (state) {
	case LI_CON_STATE_DEAD:
		return "dead";
	case LI_CON_STATE_CLOSE:
		return "close";
	case LI_CON_STATE_KEEP_ALIVE:
		return "keep-alive";
	case LI_CON_STATE_REQUEST_START:
		return "request start";
	case LI_CON_STATE_READ_REQUEST_HEADER:
		return "read request header";
	case LI_CON_STATE_HANDLE_MAINVR:
		return "handle main vrequest";
	case LI_CON_STATE_WRITE:
		return "write";
	case LI_CON_STATE_UPGRADED:
		return "upgraded";
	}

	return "undefined";
}

liConnection* li_connection_from_vrequest(liVRequest *vr) {
	liConnection *con;

	if (vr->coninfo->callbacks != &con_callbacks) return NULL;

	con = LI_CONTAINER_OF(vr->coninfo, liConnection, info);

	return con;
}

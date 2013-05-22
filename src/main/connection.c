
#include <lighttpd/base.h>
#include <lighttpd/plugin_core.h>

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
	assert(NULL != data);
	assert(NULL == data->con || data == data->con->con_sock.data);
	assert(NULL == data->sock_stream || stream == data->sock_stream);

	li_connection_simple_tcp(&data->con, stream, &data->simple_tcp_context, event);

	if (NULL != data->con && data->con->out_has_all_data
	    && (NULL == stream->stream_out.out || 0 == stream->stream_out.out->length)) {
		li_connection_request_done(data->con);
	}

	switch (event) {
	case LI_IOSTREAM_DESTROY:
		assert(NULL == data->con);
		assert(NULL == data->sock_stream);
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

static const liConnectionSocketCallbacks simple_tcp_cbs = {
	simple_tcp_finished
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
}
static void con_iostream_shutdown(liConnection *con) { /* (try) regular shutdown */
	if (con->con_sock.callbacks) {
		con->con_sock.callbacks->finish(con, FALSE);
	}
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
		li_job_later(&con->wrk->jobqueue, &con->job_reset);
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

	if (con->state == LI_CON_STATE_KEEP_ALIVE) {
		/* stop keep alive timeout watchers */
		if (con->keep_alive_data.link) {
			g_queue_delete_link(&con->wrk->keep_alive_queue, con->keep_alive_data.link);
			con->keep_alive_data.link = NULL;
		}
		con->keep_alive_data.timeout = 0;
		ev_timer_stop(con->wrk->loop, &con->keep_alive_data.watcher);

		/* put back in io timeout queue */
		if (!con->io_timeout_elem.queued)
			li_waitqueue_push(&con->wrk->io_timeout_queue, &con->io_timeout_elem);

		con->keep_alive_requests++;
		/* disable keep alive if limit is reached */
		if (con->keep_alive_requests == CORE_OPTION(LI_CORE_OPTION_MAX_KEEP_ALIVE_REQUESTS).number)
			con->info.keep_alive = FALSE;

		/* reopen stream for request body */
		li_chunkqueue_reset(in);
		/* reset stuff from keep-alive and record timestamp */
		li_vrequest_start(con->mainvr);

		con->state = LI_CON_STATE_READ_REQUEST_HEADER;
	} else if (con->state == LI_CON_STATE_REQUEST_START) {
		con->state = LI_CON_STATE_READ_REQUEST_HEADER;
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
		li_action_enter(vr, con->srv->mainaction);

		li_vrequest_handle_request_headers(vr);
	}

	if (con->state != LI_CON_STATE_READ_REQUEST_HEADER && !in->is_closed) {
		goffset newbytes = 0;

		if (vr->request.content_length == -1) {
			/* TODO: parse chunked encoded request body, filters */
			/* li_chunkqueue_steal_all(con->in, con->raw_in); */
			con->info.keep_alive = FALSE;
			in->is_closed = TRUE;
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

static void _connection_http_out_cb(liStream *stream, liStreamEvent event) {
	liConnection *con = LI_CONTAINER_OF(stream, liConnection, out);

	liChunkQueue *raw_out = stream->out, *out;
	liVRequest *vr = con->mainvr;

	switch (event) {
	case LI_STREAM_NEW_DATA:
		/* handle below */
		break;
	case LI_STREAM_CONNECTED_SOURCE:
		break;
	case LI_STREAM_DISCONNECTED_SOURCE:
		if (!con->out_has_all_data) li_connection_error(con);
		return;
	case LI_STREAM_DISCONNECTED_DEST:
		if (!raw_out->is_closed || NULL == con->con_sock.raw_out) {
			li_connection_error(con);
		} else {
			connection_close(con);
		}
		return;
	case LI_STREAM_DESTROY:
		con->info.resp = NULL;
		li_job_later(&con->wrk->jobqueue, &con->job_reset);
		return;
	default:
		return;
	}

	out = (NULL != stream->source) ? stream->source->out : NULL;

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
			li_response_send_headers(vr, raw_out, out);
		}

		if (!raw_out->is_closed && NULL != out) {
			if (vr->response.transfer_encoding & LI_HTTP_TRANSFER_ENCODING_CHUNKED) {
				li_filter_chunked_encode(vr, raw_out, out);
			} else {
				li_chunkqueue_steal_all(raw_out, out);
			}
			if (out->is_closed) {
				con->out_has_all_data = TRUE;
				out = NULL;
				li_stream_disconnect(stream);
				con->state = LI_CON_STATE_WRITE;
			}
			con->info.out_queue_length = raw_out->length;
		}
	}

	li_stream_notify(stream);
}


void li_connection_update_io_timeout(liConnection *con) {
	liWorker *wrk = con->wrk;

	if ((con->io_timeout_elem.ts + 1.0) < ev_now(wrk->loop)) {
		li_waitqueue_push(&wrk->io_timeout_queue, &con->io_timeout_elem);
	}
}

void li_connection_start(liConnection *con, liSocketAddress remote_addr, int s, liServerSocket *srv_sock) {
	assert(NULL == con->con_sock.data);

	con->srv_sock = srv_sock;
	con->state = LI_CON_STATE_REQUEST_START;
	con->mainvr->ts_started = con->ts_started = CUR_TS(con->wrk);

	con->info.remote_addr = remote_addr;
	li_sockaddr_to_string(remote_addr, con->info.remote_addr_str, FALSE);

	con->info.local_addr = li_sockaddr_dup(srv_sock->local_addr);
	li_sockaddr_to_string(con->info.local_addr, con->info.local_addr_str, FALSE);

	con->info.aborted = FALSE;

	li_stream_init(&con->in, &con->wrk->jobqueue, _connection_http_in_cb);
	li_stream_init(&con->out, &con->wrk->jobqueue, _connection_http_out_cb);

	con->info.req = &con->in;
	con->info.resp = &con->out;

	li_waitqueue_push(&con->wrk->io_timeout_queue, &con->io_timeout_elem);

	if (srv_sock->new_cb) {
		if (!srv_sock->new_cb(con, s)) {
			li_connection_error(con);
			return;
		}
	} else {
		simple_tcp_new(con, s);
	}

	assert(NULL != con->con_sock.raw_in || NULL != con->con_sock.raw_out);

	li_chunkqueue_use_limit(con->con_sock.raw_in->out, con->wrk->loop, 512*1024);
	li_chunkqueue_use_limit(con->con_sock.raw_out->out, con->wrk->loop, 512*1024);

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

static void connection_keepalive_cb(struct ev_loop *loop, ev_timer *w, int revents) {
	liConnection *con = (liConnection*) w->data;
	UNUSED(loop); UNUSED(revents);

	li_connection_reset(con);
}

static void mainvr_handle_response_error(liVRequest *vr) {
	liConnection* con = li_connection_from_vrequest(vr);
	assert(NULL != con);

	li_connection_error(con);
}

static const liConCallbacks con_callbacks = {
	mainvr_handle_response_error
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
	ev_init(&con->keep_alive_data.watcher, connection_keepalive_cb);
	con->keep_alive_data.watcher.data = con;

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
		ev_timer_stop(con->wrk->loop, &con->keep_alive_data.watcher);
		con->keep_alive_requests = 0;
	}

	li_job_later(&con->wrk->jobqueue, &con->job_reset);
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

	li_throttle_reset(con->mainvr);

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
	ev_timer_stop(con->wrk->loop, &con->keep_alive_data.watcher);
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
		ev_timer_stop(con->wrk->loop, &con->keep_alive_data.watcher);
		{
			con->keep_alive_data.max_idle = CORE_OPTION(LI_CORE_OPTION_MAX_KEEP_ALIVE_IDLE).number;
			if (con->keep_alive_data.max_idle == 0) {
				con->state = LI_CON_STATE_CLOSE;
				con_iostream_shutdown(con);
				li_connection_reset(con);
				return;
			}

			con->keep_alive_data.timeout = ev_now(con->wrk->loop) + con->keep_alive_data.max_idle;

			if (con->keep_alive_data.max_idle == con->srv->keep_alive_queue_timeout) {
				/* queue is sorted by con->keep_alive_data.timeout */
				gboolean need_start = (0 == con->wrk->keep_alive_queue.length);
				con->keep_alive_data.timeout = ev_now(con->wrk->loop) + con->srv->keep_alive_queue_timeout;
				g_queue_push_tail(&con->wrk->keep_alive_queue, con);
				con->keep_alive_data.link = g_queue_peek_tail_link(&con->wrk->keep_alive_queue);
				if (need_start)
					li_worker_check_keepalive(con->wrk);
			} else {
				ev_timer_set(&con->keep_alive_data.watcher, con->keep_alive_data.max_idle, 0);
				ev_timer_start(con->wrk->loop, &con->keep_alive_data.watcher);
			}
		}

		/* remove from timeout queue */
		li_waitqueue_remove(&con->wrk->io_timeout_queue, &con->io_timeout_elem);
	} else {
		li_stream_again_later(&con->in);
	}

	con->state = LI_CON_STATE_KEEP_ALIVE;
	con->response_headers_sent = FALSE;
	con->expect_100_cont = FALSE;
	con->out_has_all_data = FALSE;

	con->info.keep_alive = TRUE;

	con->out.out->is_closed = FALSE;

	li_throttle_reset(con->mainvr);

	li_vrequest_reset(con->mainvr, TRUE);
	li_http_request_parser_reset(&con->req_parser_ctx);

	/* restore chunkqueue limits */
	li_chunkqueue_use_limit(con->con_sock.raw_in->out, con->wrk->loop, 512*1024);
	li_chunkqueue_use_limit(con->con_sock.raw_out->out, con->wrk->loop, 512*1024);

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
	assert(NULL == con->con_sock.data);
	assert(LI_CON_STATE_DEAD == con->state);

	con->response_headers_sent = FALSE;
	con->expect_100_cont = FALSE;
	con->out_has_all_data = FALSE;

	li_server_socket_release(con->srv_sock);
	con->srv_sock = NULL;

	g_string_free(con->info.remote_addr_str, TRUE);
	li_sockaddr_clear(&con->info.remote_addr);
	g_string_free(con->info.local_addr_str, TRUE);
	li_sockaddr_clear(&con->info.local_addr);

	li_throttle_reset(con->mainvr);

	li_vrequest_free(con->mainvr);
	li_http_request_parser_clear(&con->req_parser_ctx);

	con->info.keep_alive = TRUE;
	if (con->keep_alive_data.link && con->wrk) {
		g_queue_delete_link(&con->wrk->keep_alive_queue, con->keep_alive_data.link);
		con->keep_alive_data.link = NULL;
	}
	con->keep_alive_data.timeout = 0;
	con->keep_alive_data.max_idle = 0;
	if (con->wrk)
		ev_timer_stop(con->wrk->loop, &con->keep_alive_data.watcher);

	/* remove from timeout queue */
	li_waitqueue_remove(&con->wrk->io_timeout_queue, &con->io_timeout_elem);

	li_job_clear(&con->job_reset);

	g_slice_free(liConnection, con);
}

gchar *li_connection_state_str(liConnectionState state) {
	static const gchar *states[] = {
		"dead",
		"keep-alive",
		"request start",
		"read request header",
		"handle main vrequest",
		"write"
	};

	return (gchar*)states[state];
}

liConnection* li_connection_from_vrequest(liVRequest *vr) {
	liConnection *con;

	if (vr->coninfo->callbacks != &con_callbacks) return NULL;

	con = LI_CONTAINER_OF(vr->coninfo, liConnection, info);

	return con;
}

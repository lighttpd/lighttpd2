
#include <lighttpd/base.h>
#include <lighttpd/plugin_core.h>

static void li_connection_reset_keep_alive(liConnection *con);
static void li_connection_internal_error(liConnection *con);

static void update_io_events(liConnection *con) {
	int events = 0;

	if (LI_CON_STATE_KEEP_ALIVE == con->state) {
		events = EV_READ;
	} else {
		if (!con->can_read && (con->state != LI_CON_STATE_HANDLE_MAINVR || con->mainvr->state >= LI_VRS_READ_CONTENT) && !con->in->is_closed) {
			events = events | EV_READ;
		}

		if (!con->can_write && con->raw_out->length > 0) {
			if (!con->mainvr->throttled || con->mainvr->throttle.magazine > 0) {
				events = events | EV_WRITE;
			}
		}
	}

	if (con->srv_sock->update_events_cb) {
		con->srv_sock->update_events_cb(con, events);
	} else {
		li_ev_io_set_events(con->wrk->loop, &con->sock_watcher, events);
	}
}

static void parse_request_body(liConnection *con) {
	if ((con->state > LI_CON_STATE_HANDLE_MAINVR || con->mainvr->state >= LI_VRS_READ_CONTENT) && !con->in->is_closed) {
		goffset newbytes = 0;

		if (con->mainvr->request.content_length == -1) {
			/* TODO: parse chunked encoded request body, filters */
			/* li_chunkqueue_steal_all(con->in, con->raw_in); */
			con->in->is_closed = TRUE;
		} else {
			if (con->in->bytes_in < con->mainvr->request.content_length) {
				newbytes = li_chunkqueue_steal_len(con->in, con->raw_in, con->mainvr->request.content_length - con->in->bytes_in);
			}
			if (con->in->bytes_in == con->mainvr->request.content_length) {
				con->in->is_closed = TRUE;
			}
		}
		if (newbytes > 0 || con->in->is_closed) {
			li_vrequest_handle_request_body(con->mainvr);
		}
	}
}

static void forward_response_body(liConnection *con) {
	liVRequest *vr = con->mainvr;
	if (con->state >= LI_CON_STATE_HANDLE_MAINVR) {
		if (!con->response_headers_sent) {
			if (CORE_OPTION(LI_CORE_OPTION_DEBUG_REQUEST_HANDLING).boolean) {
				VR_DEBUG(vr, "%s", "write response headers");
			}
			con->response_headers_sent = TRUE;
			if (!li_response_send_headers(con)) {
				con->response_headers_sent = FALSE;
				li_connection_internal_error(con);
				return;
			}
		}

		if (con->raw_out->is_closed) {
			li_chunkqueue_skip_all(con->out);
			con->out->is_closed = TRUE;
		} else {
			if (vr->response.transfer_encoding & LI_HTTP_TRANSFER_ENCODING_CHUNKED) {
				li_filter_chunked_encode(vr, con->raw_out, con->out);
			} else {
				li_chunkqueue_steal_all(con->raw_out, con->out);
			}
			if (con->out->is_closed) con->raw_out->is_closed = TRUE;
			con->info.out_queue_length = con->raw_out->length;
		}
	}
}

/* don't use con afterwards */
static void connection_request_done(liConnection *con) {
	liVRequest *vr = con->mainvr;
	liServerState s;

	if (CORE_OPTION(LI_CORE_OPTION_DEBUG_REQUEST_HANDLING).boolean) {
		VR_DEBUG(con->mainvr, "response end (keep_alive = %i)", con->info.keep_alive);
	}

	li_plugins_handle_close(con);

	s = g_atomic_int_get(&con->srv->dest_state);
	if (con->info.keep_alive &&  (LI_SERVER_RUNNING == s || LI_SERVER_WARMUP == s)) {
		li_connection_reset_keep_alive(con);
	} else {
		li_worker_con_put(con);
	}
}

/* return FALSE if you shouldn't use con afterwards */
static gboolean check_response_done(liConnection *con) {
	if (con->in->is_closed && con->raw_out->is_closed && 0 == con->raw_out->length) {
		connection_request_done(con);
		return FALSE;
	}
	return TRUE;
}

static void connection_close(liConnection *con) {
	liVRequest *vr = con->mainvr;
	if (CORE_OPTION(LI_CORE_OPTION_DEBUG_REQUEST_HANDLING).boolean) {
		VR_DEBUG(vr, "%s", "connection closed");
	}

	li_plugins_handle_close(con);

	li_worker_con_put(con);
}

void li_connection_error(liConnection *con) {
	liVRequest *vr = con->mainvr;
	if (CORE_OPTION(LI_CORE_OPTION_DEBUG_REQUEST_HANDLING).boolean) {
		VR_DEBUG(vr, "%s", "connection closed (error)");
	}

	li_plugins_handle_close(con);

	li_worker_con_put(con);
}

static void li_connection_internal_error(liConnection *con) {
	liVRequest *vr = con->mainvr;
	if (con->response_headers_sent) {
		if (CORE_OPTION(LI_CORE_OPTION_DEBUG_REQUEST_HANDLING).boolean) {
			VR_DEBUG(vr, "%s", "Couldn't send '500 Internal Error': headers already sent");
		}
		li_connection_error(con);
	} else {
		if (CORE_OPTION(LI_CORE_OPTION_DEBUG_REQUEST_HANDLING).boolean) {
			VR_DEBUG(vr, "%s", "internal error");
		}

		/* We only need the http version from the http request, "keep-alive" reset doesn't reset it */
		li_vrequest_reset(con->mainvr, TRUE);

		con->info.keep_alive = FALSE;
		con->mainvr->response.http_status = 500;
		con->state = LI_CON_STATE_WRITE; /* skips further vrequest handling */

		li_chunkqueue_reset(con->out);
		con->out->is_closed = TRUE;
		con->in->is_closed = TRUE;
		forward_response_body(con);
	}
}

static gboolean connection_handle_read(liConnection *con) {
	liVRequest *vr = con->mainvr;

	if (con->raw_in->length == 0) return TRUE;

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

		con->state = LI_CON_STATE_READ_REQUEST_HEADER;

		li_vrequest_start(con->mainvr);
	} else {
		if (con->state == LI_CON_STATE_REQUEST_START)
			con->state = LI_CON_STATE_READ_REQUEST_HEADER;
	}

	if (con->state == LI_CON_STATE_READ_REQUEST_HEADER && con->mainvr->state == LI_VRS_CLEAN) {
		liHandlerResult res;

		if (CORE_OPTION(LI_CORE_OPTION_DEBUG_REQUEST_HANDLING).boolean) {
			VR_DEBUG(vr, "%s", "reading request header");
		}

		res = li_http_request_parse(con->mainvr, &con->req_parser_ctx);

		/* max uri length 8 kilobytes */
		if (vr->request.uri.raw->len > 8*1024) {
			VR_INFO(vr,
				"request uri too large. limit: 8kb, received: %s",
				li_counter_format(vr->request.uri.raw->len, COUNTER_BYTES, vr->wrk->tmp_str)->str
			);

			con->info.keep_alive = FALSE;
			con->mainvr->response.http_status = 414; /* Request-URI Too Large */
			li_vrequest_handle_direct(con->mainvr);
			con->state = LI_CON_STATE_WRITE;
			con->in->is_closed = TRUE;
			forward_response_body(con);
			return TRUE;
		}

		switch(res) {
		case LI_HANDLER_GO_ON:
			break; /* go on */
		case LI_HANDLER_WAIT_FOR_EVENT:
			return TRUE;
		case LI_HANDLER_ERROR:
		case LI_HANDLER_COMEBACK: /* unexpected */
			/* unparsable header */
			if (CORE_OPTION(LI_CORE_OPTION_DEBUG_REQUEST_HANDLING).boolean) {
				VR_DEBUG(vr, "%s", "parsing header failed");
			}

			con->wrk->stats.requests++;
			con->info.keep_alive = FALSE;
			/* set status 400 if not already set to e.g. 413 */
			if (con->mainvr->response.http_status == 0)
				con->mainvr->response.http_status = 400;
			li_vrequest_handle_direct(con->mainvr);
			con->state = LI_CON_STATE_WRITE;
			con->in->is_closed = TRUE;
			forward_response_body(con);
			return TRUE;
		}

		con->wrk->stats.requests++;

		/* headers ready */
		if (CORE_OPTION(LI_CORE_OPTION_DEBUG_REQUEST_HANDLING).boolean) {
			VR_DEBUG(vr, "%s", "validating request header");
		}
		if (!li_request_validate_header(con)) {
			/* skip mainvr handling */
			con->state = LI_CON_STATE_WRITE;
			con->info.keep_alive = FALSE;
			con->in->is_closed = TRUE;
			forward_response_body(con);
		} else {
			/* When does a client ask for 100 Continue? probably not while trying to ddos us
			 * as post content probably goes to a dynamic backend anyway, we don't
			 * care about the rare cases we could determine that we don't want a request at all
			 * before sending it to a backend - so just send the stupid header
			 */
			if (con->expect_100_cont) {
				if (CORE_OPTION(LI_CORE_OPTION_DEBUG_REQUEST_HANDLING).boolean) {
					VR_DEBUG(vr, "%s", "send 100 Continue");
				}
				li_chunkqueue_append_mem(con->raw_out, CONST_STR_LEN("HTTP/1.1 100 Continue\r\n\r\n"));
				con->expect_100_cont = FALSE;
			}

			con->state = LI_CON_STATE_HANDLE_MAINVR;
			li_action_enter(con->mainvr, con->srv->mainaction);
			li_vrequest_handle_request_headers(con->mainvr);
		}
	} else {
		parse_request_body(con);
	}

	return TRUE;
}

static void connection_update_io_timeout(liConnection *con) {
	liWorker *wrk = con->wrk;

	if ((con->io_timeout_elem.ts + 1.0) < ev_now(wrk->loop)) {
		li_waitqueue_push(&wrk->io_timeout_queue, &con->io_timeout_elem);
	}
}

static gboolean connection_try_read(liConnection *con) {
	liNetworkStatus res;

	/* con->can_read = TRUE; */

	if (!con->in->is_closed) {
		goffset transferred;
		transferred = con->raw_in->length;

		if (con->srv_sock->read_cb) {
			res = con->srv_sock->read_cb(con);
		} else {
			res = li_network_read(con->mainvr, con->sock_watcher.fd, con->raw_in, &con->raw_in_buffer);
		}

		transferred = con->raw_in->length - transferred;
		if (transferred > 0) connection_update_io_timeout(con);

		li_vrequest_update_stats_in(con->mainvr, transferred);

		switch (res) {
		case LI_NETWORK_STATUS_SUCCESS:
			con->can_read = FALSE; /* for now we still need the EV_READ event to get a callback */
			if (!connection_handle_read(con)) return FALSE;
			break;
		case LI_NETWORK_STATUS_FATAL_ERROR:
			_ERROR(con->srv, con->mainvr, "%s", "network read fatal error");
			li_connection_error(con);
			return FALSE;
		case LI_NETWORK_STATUS_CONNECTION_CLOSE:
			con->raw_in->is_closed = TRUE;
			/* shutdown(con->sock_watcher.fd, SHUT_RD); */ /* useless anyway */
			ev_io_stop(con->wrk->loop, &con->sock_watcher);
			close(con->sock_watcher.fd);
			ev_io_set(&con->sock_watcher, -1, 0);
			connection_close(con);
			return FALSE;
		case LI_NETWORK_STATUS_WAIT_FOR_EVENT:
			con->can_read = FALSE;
			break;
		}
	}

	return TRUE;
}

static gboolean connection_try_write(liConnection *con) {
	liNetworkStatus res;

	con->can_write = TRUE;

	if (con->raw_out->length > 0) {
		goffset transferred;
		static const goffset WRITE_MAX = 256*1024; /* 256kB */
		goffset write_max;

		if (con->mainvr->throttled) {
			write_max = MIN(con->mainvr->throttle.magazine, WRITE_MAX);
		} else {
			write_max = WRITE_MAX;
		}

		if (write_max > 0) {
			transferred = con->raw_out->length;

			if (con->srv_sock->write_cb) {
				res = con->srv_sock->write_cb(con, write_max);
			} else {
				res = li_network_write(con->mainvr, con->sock_watcher.fd, con->raw_out, write_max);
			}

			transferred = transferred - con->raw_out->length;
			con->info.out_queue_length = con->raw_out->length;
			if (transferred > 0) {
				connection_update_io_timeout(con);
				li_vrequest_joblist_append(con->mainvr);
			}
			con->can_write = FALSE; /* for now we still need the EV_WRITE event to get a callback */

			switch (res) {
			case LI_NETWORK_STATUS_SUCCESS:
				break;
			case LI_NETWORK_STATUS_FATAL_ERROR:
				_ERROR(con->srv, con->mainvr, "%s", "network write fatal error");
				li_connection_error(con);
				return FALSE;
			case LI_NETWORK_STATUS_CONNECTION_CLOSE:
				connection_close(con);
				return FALSE;
			case LI_NETWORK_STATUS_WAIT_FOR_EVENT:
				break;
			}
		} else {
			transferred = 0;
		}

		li_vrequest_update_stats_out(con->mainvr, transferred);

		if (con->mainvr->throttled) {
			li_throttle_update(con->mainvr, transferred, WRITE_MAX);
		}
	}

	return TRUE;
}

void connection_handle_io(liConnection *con) {
	/* ensure that the connection is always in the io timeout queue */
	if (!con->io_timeout_elem.queued)
		li_waitqueue_push(&con->wrk->io_timeout_queue, &con->io_timeout_elem);

	if (con->can_read)
		if (!connection_try_read(con)) return;
	if (con->can_write)
		if (!connection_try_write(con)) return;

	if (!check_response_done(con)) return;

	update_io_events(con);
}

static void connection_cb(struct ev_loop *loop, ev_io *w, int revents) {
	liConnection *con = (liConnection*) w->data;
	UNUSED(loop);

	if (revents & EV_READ) con->can_read = TRUE;
	if (revents & EV_WRITE) con->can_write = TRUE;

	if (revents & EV_ERROR) {
		/* if this happens, we have a serious bug in the event handling */
		VR_ERROR(con->mainvr, "%s", "EV_ERROR encountered, dropping connection!");
		li_connection_error(con);
		return;
	}

	connection_handle_io(con);
}

static void connection_keepalive_cb(struct ev_loop *loop, ev_timer *w, int revents) {
	liConnection *con = (liConnection*) w->data;
	UNUSED(loop); UNUSED(revents);
	li_worker_con_put(con);
}

static liHandlerResult mainvr_handle_response_headers(liVRequest *vr) {
	liConnection *con = LI_CONTAINER_OF(vr->coninfo, liConnection, info);
	if (CORE_OPTION(LI_CORE_OPTION_DEBUG_REQUEST_HANDLING).boolean) {
		VR_DEBUG(vr, "%s", "read request/handle response header");
	}

	if (con->can_read)
		if (!connection_try_read(con)) return FALSE;

	parse_request_body(con);

	if (con->can_write)
		if (!connection_try_write(con)) return FALSE;

	update_io_events(con);

	return LI_HANDLER_GO_ON;
}

static liHandlerResult mainvr_handle_response_body(liVRequest *vr) {
	liConnection *con = LI_CONTAINER_OF(vr->coninfo, liConnection, info);
	if (!check_response_done(con)) return LI_HANDLER_GO_ON;

	if (CORE_OPTION(LI_CORE_OPTION_DEBUG_REQUEST_HANDLING).boolean) {
		VR_DEBUG(vr, "%s", "write response");
	}

	if (con->can_read)
		if (!connection_try_read(con)) return FALSE;

	parse_request_body(con);
	forward_response_body(con);

	if (con->can_write)
		if (!connection_try_write(con)) return FALSE;

	if (!check_response_done(con)) return LI_HANDLER_GO_ON;

	update_io_events(con);

	return LI_HANDLER_GO_ON;
}

static liHandlerResult mainvr_handle_response_error(liVRequest *vr) {
	liConnection *con = LI_CONTAINER_OF(vr->coninfo, liConnection, info);

	li_connection_internal_error(con);

	if (con->can_read)
		if (!connection_try_read(con)) return FALSE;
	if (con->can_write)
		if (!connection_try_write(con)) return FALSE;

	update_io_events(con);

	return LI_HANDLER_GO_ON;
}

static liHandlerResult mainvr_handle_request_headers(liVRequest *vr) {
	liConnection *con = LI_CONTAINER_OF(vr->coninfo, liConnection, info);

	/* start reading input */
	if (con->can_read)
		if (!connection_try_read(con)) return FALSE;

	parse_request_body(con);
	update_io_events(con);

	return LI_HANDLER_GO_ON;
}

static gboolean mainvr_handle_check_io(liVRequest *vr) {
	liConnection *con = LI_CONTAINER_OF(vr->coninfo, liConnection, info);

	if (con->can_read)
		if (!connection_try_read(con)) return FALSE;
	if (con->can_write)
		if (!connection_try_write(con)) return FALSE;

	update_io_events(con);

	return TRUE;
}

static const liConCallbacks con_callbacks = {
	mainvr_handle_request_headers,
	mainvr_handle_response_headers,
	mainvr_handle_response_body,
	mainvr_handle_response_error,

	mainvr_handle_check_io
};

liConnection* li_connection_new(liWorker *wrk) {
	liServer *srv = wrk->srv;
	liConnection *con = g_slice_new0(liConnection);
	con->wrk = wrk;
	con->srv = srv;

	con->state = LI_CON_STATE_DEAD;
	con->response_headers_sent = FALSE;
	con->expect_100_cont = FALSE;

	ev_init(&con->sock_watcher, connection_cb);
	ev_io_set(&con->sock_watcher, -1, 0);
	con->sock_watcher.data = con;
	con->info.remote_addr_str = g_string_sized_new(INET6_ADDRSTRLEN);
	con->info.local_addr_str = g_string_sized_new(INET6_ADDRSTRLEN);
	con->info.is_ssl = FALSE;
	con->info.keep_alive = TRUE;

	con->raw_in  = li_chunkqueue_new();
	con->raw_out = li_chunkqueue_new();

	con->info.callbacks = &con_callbacks;

	con->mainvr = li_vrequest_new(wrk, &con->info);
	li_http_request_parser_init(&con->req_parser_ctx, &con->mainvr->request, con->raw_in);

	con->in      = con->mainvr->vr_in;
	con->out     = con->mainvr->vr_out;

	li_chunkqueue_set_limit(con->raw_in, con->in->limit);
	li_chunkqueue_set_limit(con->raw_out, con->out->limit);
	li_cqlimit_set_limit(con->raw_in->limit, 512*1024);
	li_cqlimit_set_limit(con->raw_out->limit, 512*1024);

	con->keep_alive_data.link = NULL;
	con->keep_alive_data.timeout = 0;
	con->keep_alive_data.max_idle = 0;
	ev_init(&con->keep_alive_data.watcher, connection_keepalive_cb);
	con->keep_alive_data.watcher.data = con;

	con->can_read = con->can_write = TRUE;

	con->io_timeout_elem.data = con;

	return con;
}

void li_connection_reset(liConnection *con) {
	con->state = LI_CON_STATE_DEAD;
	con->response_headers_sent = FALSE;
	con->expect_100_cont = FALSE;

	if (con->srv_sock->close_cb)
		con->srv_sock->close_cb(con);

	li_server_socket_release(con->srv_sock);
	con->srv_sock = NULL;
	con->srv_sock_data = NULL;
	con->info.is_ssl = FALSE;

	ev_io_stop(con->wrk->loop, &con->sock_watcher);
	if (con->sock_watcher.fd != -1) {
		if (con->raw_in->is_closed) { /* read already got EOF */
			shutdown(con->sock_watcher.fd, SHUT_RDWR);
			close(con->sock_watcher.fd);
		} else {
			li_worker_add_closing_socket(con->wrk, con->sock_watcher.fd);
		}
	}
	ev_io_set(&con->sock_watcher, -1, 0);
	ev_set_cb(&con->sock_watcher, connection_cb);

	li_chunkqueue_reset(con->raw_in);
	li_chunkqueue_reset(con->raw_out);
	con->info.out_queue_length = 0;
	li_buffer_release(con->raw_in_buffer);
	con->raw_in_buffer = NULL;

	li_throttle_reset(con->mainvr);

	li_vrequest_reset(con->mainvr, FALSE);

	/* restore chunkqueue limits */
	li_chunkqueue_set_limit(con->raw_in, con->in->limit);
	li_chunkqueue_set_limit(con->raw_out, con->out->limit);
	li_cqlimit_reset(con->raw_in->limit);
	li_cqlimit_reset(con->raw_out->limit);
	li_cqlimit_set_limit(con->raw_in->limit, 512*1024);
	li_cqlimit_set_limit(con->raw_out->limit, 512*1024);

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

	con->can_read = con->can_write = TRUE;

	/* remove from timeout queue */
	li_waitqueue_remove(&con->wrk->io_timeout_queue, &con->io_timeout_elem);
}

static void li_connection_reset_keep_alive(liConnection *con) {
	liVRequest *vr = con->mainvr;

	/* only start keep alive watcher if there isn't more input data already */
	if (con->raw_in->length == 0) {
		ev_timer_stop(con->wrk->loop, &con->keep_alive_data.watcher);
		{
			con->keep_alive_data.max_idle = CORE_OPTION(LI_CORE_OPTION_MAX_KEEP_ALIVE_IDLE).number;
			if (con->keep_alive_data.max_idle == 0) {
				li_worker_con_put(con);
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
	}

	con->state = LI_CON_STATE_KEEP_ALIVE;
	con->response_headers_sent = FALSE;
	con->expect_100_cont = FALSE;

	update_io_events(con);
	con->info.keep_alive = TRUE;

	con->raw_out->is_closed = FALSE;
	con->info.out_queue_length = con->raw_out->length;

	li_throttle_reset(con->mainvr);

	li_vrequest_reset(con->mainvr, TRUE);
	li_http_request_parser_reset(&con->req_parser_ctx);

	/* restore chunkqueue limits (don't reset, we might still have some data in raw_in) */
	li_chunkqueue_set_limit(con->raw_in, con->in->limit);
	li_chunkqueue_set_limit(con->raw_out, con->out->limit);
	li_cqlimit_set_limit(con->raw_in->limit, 512*1024);
	li_cqlimit_set_limit(con->raw_out->limit, 512*1024);

	/* reset stats */
	con->info.stats.bytes_in = G_GUINT64_CONSTANT(0);
	con->info.stats.bytes_in_5s = G_GUINT64_CONSTANT(0);
	con->info.stats.bytes_in_5s_diff = G_GUINT64_CONSTANT(0);
	con->info.stats.bytes_out = G_GUINT64_CONSTANT(0);
	con->info.stats.bytes_out_5s = G_GUINT64_CONSTANT(0);
	con->info.stats.bytes_out_5s_diff = G_GUINT64_CONSTANT(0);
	con->info.stats.last_avg = 0;

	if (con->raw_in->length != 0) {
		/* start handling next request if data is already available */
		connection_handle_read(con);
	}
}

void li_connection_free(liConnection *con) {
	con->state = LI_CON_STATE_DEAD;
	con->response_headers_sent = FALSE;
	con->expect_100_cont = FALSE;

	li_server_socket_release(con->srv_sock);
	con->srv_sock = NULL;

	if (con->wrk)
		ev_io_stop(con->wrk->loop, &con->sock_watcher);
	if (con->sock_watcher.fd != -1) {
		/* just close it; _free should only be called on dead connections anyway */
		shutdown(con->sock_watcher.fd, SHUT_WR);
		close(con->sock_watcher.fd);
	}
	ev_io_set(&con->sock_watcher, -1, 0);
	g_string_free(con->info.remote_addr_str, TRUE);
	li_sockaddr_clear(&con->info.remote_addr);
	g_string_free(con->info.local_addr_str, TRUE);
	li_sockaddr_clear(&con->info.local_addr);
	con->info.keep_alive = TRUE;

	li_chunkqueue_free(con->raw_in);
	li_chunkqueue_free(con->raw_out);
	li_buffer_release(con->raw_in_buffer);

	li_throttle_reset(con->mainvr);

	li_vrequest_free(con->mainvr);
	li_http_request_parser_clear(&con->req_parser_ctx);

	if (con->keep_alive_data.link && con->wrk) {
		g_queue_delete_link(&con->wrk->keep_alive_queue, con->keep_alive_data.link);
		con->keep_alive_data.link = NULL;
	}
	con->keep_alive_data.timeout = 0;
	con->keep_alive_data.max_idle = 0;
	if (con->wrk)
		ev_timer_stop(con->wrk->loop, &con->keep_alive_data.watcher);

	g_slice_free(liConnection, con);
}

void li_connection_start(liConnection *con, liSocketAddress remote_addr, int s, liServerSocket *srv_sock) {
	ev_io_set(&con->sock_watcher, s, 0);

	con->srv_sock = srv_sock;
	con->state = LI_CON_STATE_REQUEST_START;
	con->mainvr->ts_started = con->ts_started = CUR_TS(con->wrk);

	con->info.remote_addr = remote_addr;
	li_sockaddr_to_string(remote_addr, con->info.remote_addr_str, FALSE);

	con->info.local_addr = li_sockaddr_local_from_socket(s);
	li_sockaddr_to_string(con->info.local_addr, con->info.local_addr_str, FALSE);

	li_waitqueue_push(&con->wrk->io_timeout_queue, &con->io_timeout_elem);

	if (srv_sock->new_cb) {
		if (!srv_sock->new_cb(con)) {
			li_connection_error(con);
			return;
		}
	}

	if (con->can_read)
		if (!connection_try_read(con)) return;

	update_io_events(con);
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

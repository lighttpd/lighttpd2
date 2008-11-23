
#include <lighttpd/base.h>
#include <lighttpd/plugin_core.h>

/* only call it from the worker context the con belongs to */
void worker_con_put(connection *con); /* worker.c */

static void parse_request_body(connection *con) {
	if ((con->state > CON_STATE_HANDLE_MAINVR || con->mainvr->state >= VRS_READ_CONTENT) && !con->in->is_closed) {
		ev_io_add_events(con->wrk->loop, &con->sock_watcher, EV_READ);
		if (con->mainvr->request.content_length == -1) {
			/* TODO: parse chunked encoded request body, filters */
			chunkqueue_steal_all(con->in, con->raw_in);
		} else {
			if (con->in->bytes_in < con->mainvr->request.content_length) {
				chunkqueue_steal_len(con->in, con->raw_in, con->mainvr->request.content_length - con->in->bytes_in);
			}
			if (con->in->bytes_in == con->mainvr->request.content_length) {
				con->in->is_closed = TRUE;
				ev_io_rem_events(con->wrk->loop, &con->sock_watcher, EV_READ);
			}
		}
	} else {
		ev_io_rem_events(con->wrk->loop, &con->sock_watcher, EV_READ);
	}
}

static void forward_response_body(connection *con) {
	vrequest *vr = con->mainvr;
	if (con->state >= CON_STATE_HANDLE_MAINVR) {
		if (!con->response_headers_sent) {
			con->response_headers_sent = TRUE;
			if (CORE_OPTION(CORE_OPTION_DEBUG_REQUEST_HANDLING).boolean) {
				CON_TRACE(con, "%s", "write response headers");
			}
			response_send_headers(con);
		}

		chunkqueue_steal_all(con->raw_out, con->out);
		if (con->out->is_closed) con->raw_out->is_closed = TRUE;
		if (con->raw_out->length > 0) {
			ev_io_add_events(con->wrk->loop, &con->sock_watcher, EV_WRITE);
		} else {
			ev_io_rem_events(con->wrk->loop, &con->sock_watcher, EV_WRITE);
		}
	} else {
		ev_io_rem_events(con->wrk->loop, &con->sock_watcher, EV_WRITE);
	}
}

static void connection_request_done(connection *con) {
	vrequest *vr = con->mainvr;
	if (CORE_OPTION(CORE_OPTION_DEBUG_REQUEST_HANDLING).boolean) {
		CON_TRACE(con, "response end (keep_alive = %i)", con->keep_alive);
	}

	plugins_handle_close(con);

	if (con->keep_alive) {
		connection_reset_keep_alive(con);
	} else {
		worker_con_put(con);
	}
}

static gboolean check_response_done(connection *con) {
	if (con->in->is_closed && con->raw_out->is_closed && 0 == con->raw_out->length) {
		connection_request_done(con);
		return TRUE;
	}
	return FALSE;
}

static void connection_close(connection *con) {
	vrequest *vr = con->mainvr;
	if (CORE_OPTION(CORE_OPTION_DEBUG_REQUEST_HANDLING).boolean) {
		CON_TRACE(con, "%s", "connection closed");
	}

	plugins_handle_close(con);

	worker_con_put(con);
}

void connection_error(connection *con) {
	vrequest *vr = con->mainvr;
	if (CORE_OPTION(CORE_OPTION_DEBUG_REQUEST_HANDLING).boolean) {
		CON_TRACE(con, "%s", "connection closed (error)");
	}

	plugins_handle_close(con);

	worker_con_put(con);
}

void connection_internal_error(connection *con) {
	vrequest *vr = con->mainvr;
	if (con->response_headers_sent) {
		VR_ERROR(vr, "%s", "Couldn't send '500 Internal Error': headers already sent");
		connection_error(con);
	} else {
		vrequest_reset(con->mainvr);
		http_headers_reset(con->mainvr->response.headers);
		VR_ERROR(vr, "%s", "internal error");
		con->mainvr->response.http_status = 500;
		con->state = CON_STATE_WRITE;
		forward_response_body(con);
	}
}

static gboolean connection_handle_read(connection *con) {
	vrequest *vr = con->mainvr;

	if (con->raw_in->length == 0) return TRUE;

	if (con->state == CON_STATE_KEEP_ALIVE) {
		/* stop keep alive timeout watchers */
		if (con->keep_alive_data.link) {
			g_queue_delete_link(&con->wrk->keep_alive_queue, con->keep_alive_data.link);
			con->keep_alive_data.link = NULL;
		}
		con->keep_alive_data.timeout = 0;
		ev_timer_stop(con->wrk->loop, &con->keep_alive_data.watcher);

		con->state = CON_STATE_READ_REQUEST_HEADER;
		con->ts = CUR_TS(con->wrk);

	} else {
		if (con->state == CON_STATE_REQUEST_START)
			con->state = CON_STATE_READ_REQUEST_HEADER;
	}

	if (con->state == CON_STATE_READ_REQUEST_HEADER && con->mainvr->state == VRS_CLEAN) {
		if (CORE_OPTION(CORE_OPTION_DEBUG_REQUEST_HANDLING).boolean) {
			CON_TRACE(con, "%s", "reading request header");
		}
		switch(http_request_parse(con->mainvr, &con->req_parser_ctx)) {
		case HANDLER_FINISHED:
		case HANDLER_GO_ON:
			break; /* go on */
		case HANDLER_WAIT_FOR_EVENT:
			return TRUE;
		case HANDLER_ERROR:
		case HANDLER_COMEBACK: /* unexpected */
		case HANDLER_WAIT_FOR_FD: /* unexpected */
			/* unparsable header */
			connection_error(con);
			return FALSE;
		}

		con->wrk->stats.requests++;

		/* headers ready */
		if (CORE_OPTION(CORE_OPTION_DEBUG_REQUEST_HANDLING).boolean) {
			CON_TRACE(con, "%s", "validating request header");
		}
		if (!request_validate_header(con)) {
			/* skip mainvr handling */
			con->state = CON_STATE_WRITE;
			con->keep_alive = FALSE;
			con->in->is_closed = TRUE;
			forward_response_body(con);
		} else {
			con->state = CON_STATE_HANDLE_MAINVR;
			action_enter(con->mainvr, con->srv->mainaction);
			vrequest_handle_request_headers(con->mainvr);
		}
	}

	return TRUE;
}

static void connection_cb(struct ev_loop *loop, ev_io *w, int revents) {
	connection *con = (connection*) w->data;

	if (revents & EV_READ) {
		if (con->in->is_closed) {
			/* don't read the next request before current one is done */
			ev_io_rem_events(loop, w, EV_READ);
		} else {
			switch (network_read(con->mainvr, w->fd, con->raw_in)) {
			case NETWORK_STATUS_SUCCESS:
				if (!connection_handle_read(con)) return;
				break;
			case NETWORK_STATUS_FATAL_ERROR:
				CON_ERROR(con, "%s", "network read fatal error");
				connection_error(con);
				return;
			case NETWORK_STATUS_CONNECTION_CLOSE:
				con->raw_in->is_closed = TRUE;
				shutdown(w->fd, SHUT_RD);
				connection_close(con);
				return;
			case NETWORK_STATUS_WAIT_FOR_EVENT:
				break;
			case NETWORK_STATUS_WAIT_FOR_AIO_EVENT:
				/* TODO: aio */
				ev_io_rem_events(loop, w, EV_READ);
				break;
			case NETWORK_STATUS_WAIT_FOR_FD:
				/* TODO: wait for fd */
				ev_io_rem_events(loop, w, EV_READ);
				break;
			}
		}
	}

	if (revents & EV_WRITE) {
		if (con->raw_out->length > 0) {
			switch (network_write(con->mainvr, w->fd, con->raw_out)) {
			case NETWORK_STATUS_SUCCESS:
				vrequest_joblist_append(con->mainvr);
				break;
			case NETWORK_STATUS_FATAL_ERROR:
				CON_ERROR(con, "%s", "network write fatal error");
				connection_error(con);
				break;
			case NETWORK_STATUS_CONNECTION_CLOSE:
				connection_close(con);
				return;
			case NETWORK_STATUS_WAIT_FOR_EVENT:
				break;
			case NETWORK_STATUS_WAIT_FOR_AIO_EVENT:
				ev_io_rem_events(loop, w, EV_WRITE);
				CON_ERROR(con, "%s", "TODO: wait for aio");
				/* TODO: aio */
				break;
			case NETWORK_STATUS_WAIT_FOR_FD:
				ev_io_rem_events(loop, w, EV_WRITE);
				CON_ERROR(con, "%s", "TODO: wait for fd");
				/* TODO: wait for fd */
				break;
			}
		} else {
			CON_TRACE(con, "%s", "write event for empty queue");
			ev_io_rem_events(loop, w, EV_WRITE);
		}
	}

	check_response_done(con);
}

static void connection_keepalive_cb(struct ev_loop *loop, ev_timer *w, int revents) {
	connection *con = (connection*) w->data;
	UNUSED(loop); UNUSED(revents);
	worker_con_put(con);
}

static handler_t mainvr_handle_response_headers(vrequest *vr) {
	connection *con = vr->con;
	if (CORE_OPTION(CORE_OPTION_DEBUG_REQUEST_HANDLING).boolean) {
		VR_TRACE(vr, "%s", "read request/handle response header");
	}
	if (con->expect_100_cont) {
		if (CORE_OPTION(CORE_OPTION_DEBUG_REQUEST_HANDLING).boolean) {
			VR_TRACE(vr, "%s", "send 100 Continue");
		}
		chunkqueue_append_mem(con->raw_out, CONST_STR_LEN("HTTP/1.1 100 Continue\r\n\r\n"));
		con->expect_100_cont = FALSE;
		ev_io_add_events(con->wrk->loop, &con->sock_watcher, EV_WRITE);
	}
	parse_request_body(con);

	return HANDLER_GO_ON;
}

static handler_t mainvr_handle_response_body(vrequest *vr) {
	connection *con = vr->con;
	if (check_response_done(con)) return HANDLER_FINISHED;

	if (CORE_OPTION(CORE_OPTION_DEBUG_REQUEST_HANDLING).boolean) {
		CON_TRACE(con, "%s", "write response");
	}

	parse_request_body(con);
	forward_response_body(con);

	if (check_response_done(con)) return HANDLER_FINISHED;

	return HANDLER_GO_ON;
}

static handler_t mainvr_handle_response_error(vrequest *vr) {
	connection_internal_error(vr->con);
	return HANDLER_FINISHED;
}

static handler_t mainvr_handle_request_headers(vrequest *vr) {
	/* start reading input */
	parse_request_body(vr->con);
	return HANDLER_GO_ON;
}

connection* connection_new(worker *wrk) {
	server *srv = wrk->srv;
	connection *con = g_slice_new0(connection);
	con->wrk = wrk;
	con->srv = srv;

	con->state = CON_STATE_DEAD;
	con->response_headers_sent = FALSE;
	con->expect_100_cont = FALSE;

	ev_init(&con->sock_watcher, connection_cb);
	ev_io_set(&con->sock_watcher, -1, 0);
	con->sock_watcher.data = con;
	con->remote_addr_str = g_string_sized_new(INET6_ADDRSTRLEN);
	con->local_addr_str = g_string_sized_new(INET6_ADDRSTRLEN);
	con->keep_alive = TRUE;

	con->raw_in  = chunkqueue_new();
	con->raw_out = chunkqueue_new();

	con->options = g_slice_copy(srv->option_def_values->len * sizeof(option_value), srv->option_def_values->data);

	con->mainvr = vrequest_new(con,
		mainvr_handle_response_headers,
		mainvr_handle_response_body,
		mainvr_handle_response_error,
		mainvr_handle_request_headers);
	http_request_parser_init(&con->req_parser_ctx, &con->mainvr->request, con->raw_in);

	con->in      = con->mainvr->vr_in;
	con->out     = con->mainvr->vr_out;

	con->keep_alive_data.link = NULL;
	con->keep_alive_data.timeout = 0;
	con->keep_alive_data.max_idle = 0;
	ev_init(&con->keep_alive_data.watcher, connection_keepalive_cb);
	con->keep_alive_data.watcher.data = con;

	con->io_timeout_elem.data = con;
	con->throttle.queue_elem.data = con;

	return con;
}

void connection_reset(connection *con) {
	con->state = CON_STATE_DEAD;
	con->response_headers_sent = FALSE;
	con->expect_100_cont = FALSE;

	ev_io_stop(con->wrk->loop, &con->sock_watcher);
	if (con->sock_watcher.fd != -1) {
		if (con->raw_in->is_closed) { /* read already shutdown */
			shutdown(con->sock_watcher.fd, SHUT_WR);
			close(con->sock_watcher.fd);
		} else {
			worker_add_closing_socket(con->wrk, con->sock_watcher.fd);
		}
	}
	ev_io_set(&con->sock_watcher, -1, 0);
	g_string_truncate(con->remote_addr_str, 0);
	g_string_truncate(con->local_addr_str, 0);
	con->keep_alive = TRUE;

	chunkqueue_reset(con->raw_in);
	chunkqueue_reset(con->raw_out);

	memcpy(con->options, con->srv->option_def_values->data, con->srv->option_def_values->len * sizeof(option_value));

	vrequest_reset(con->mainvr);
	http_request_parser_reset(&con->req_parser_ctx);

	if (con->keep_alive_data.link) {
		g_queue_delete_link(&con->wrk->keep_alive_queue, con->keep_alive_data.link);
		con->keep_alive_data.link = NULL;
	}
	con->keep_alive_data.timeout = 0;
	con->keep_alive_data.max_idle = 0;
	ev_timer_stop(con->wrk->loop, &con->keep_alive_data.watcher);

	/* reset stats */
	con->stats.bytes_in = G_GUINT64_CONSTANT(0);
	con->stats.bytes_in_5s = G_GUINT64_CONSTANT(0);
	con->stats.bytes_in_5s_diff = G_GUINT64_CONSTANT(0);
	con->stats.bytes_out = G_GUINT64_CONSTANT(0);
	con->stats.bytes_out_5s = G_GUINT64_CONSTANT(0);
	con->stats.bytes_out_5s_diff = G_GUINT64_CONSTANT(0);
	con->stats.last_avg = 0;

	/* remove from timeout queue */
	waitqueue_remove(&con->wrk->io_timeout_queue, &con->io_timeout_elem);
	/* remove from throttle queue */
	waitqueue_remove(&con->wrk->throttle_queue, &con->throttle.queue_elem);
}

void server_check_keepalive(server *srv);
void connection_reset_keep_alive(connection *con) {
	vrequest *vr = con->mainvr;
	ev_timer_stop(con->wrk->loop, &con->keep_alive_data.watcher);
	{
		con->keep_alive_data.max_idle = CORE_OPTION(CORE_OPTION_MAX_KEEP_ALIVE_IDLE).number;
		if (con->keep_alive_data.max_idle == 0) {
			worker_con_put(con);
			return;
		}
		if (con->keep_alive_data.max_idle >= con->srv->keep_alive_queue_timeout) {
			/* queue is sorted by con->keep_alive_data.timeout */
			gboolean need_start = (0 == con->wrk->keep_alive_queue.length);
			con->keep_alive_data.timeout = ev_now(con->wrk->loop) + con->srv->keep_alive_queue_timeout;
			g_queue_push_tail(&con->wrk->keep_alive_queue, con);
			con->keep_alive_data.link = g_queue_peek_tail_link(&con->wrk->keep_alive_queue);
			if (need_start)
				worker_check_keepalive(con->wrk);
		} else {
			ev_timer_set(&con->keep_alive_data.watcher, con->keep_alive_data.max_idle, 0);
			ev_timer_start(con->wrk->loop, &con->keep_alive_data.watcher);
		}
	}

	con->state = CON_STATE_KEEP_ALIVE;
	con->response_headers_sent = FALSE;
	con->expect_100_cont = FALSE;

	ev_io_set_events(con->wrk->loop, &con->sock_watcher, EV_READ);
	con->keep_alive = TRUE;

	con->raw_out->is_closed = FALSE;

	memcpy(con->options, con->srv->option_def_values->data, con->srv->option_def_values->len * sizeof(option_value));

	vrequest_reset(con->mainvr);
	http_request_parser_reset(&con->req_parser_ctx);

	con->ts = CUR_TS(con->wrk);

	/* reset stats */
	con->stats.bytes_in = G_GUINT64_CONSTANT(0);
	con->stats.bytes_in_5s = G_GUINT64_CONSTANT(0);
	con->stats.bytes_in_5s_diff = G_GUINT64_CONSTANT(0);
	con->stats.bytes_out = G_GUINT64_CONSTANT(0);
	con->stats.bytes_out_5s = G_GUINT64_CONSTANT(0);
	con->stats.bytes_out_5s_diff = G_GUINT64_CONSTANT(0);
	con->stats.last_avg = 0;

	/* remove from timeout queue */
	waitqueue_remove(&con->wrk->io_timeout_queue, &con->io_timeout_elem);
	/* remove from throttle queue */
	waitqueue_remove(&con->wrk->throttle_queue, &con->throttle.queue_elem);
}

void connection_free(connection *con) {
	con->state = CON_STATE_DEAD;
	con->response_headers_sent = FALSE;
	con->expect_100_cont = FALSE;

	if (con->wrk)
		ev_io_stop(con->wrk->loop, &con->sock_watcher);
	if (con->sock_watcher.fd != -1) {
		/* just close it; _free should only be called on dead connections anyway */
		shutdown(con->sock_watcher.fd, SHUT_WR);
		close(con->sock_watcher.fd);
	}
	ev_io_set(&con->sock_watcher, -1, 0);
	g_string_free(con->remote_addr_str, TRUE);
	g_string_free(con->local_addr_str, TRUE);
	con->keep_alive = TRUE;

	chunkqueue_free(con->raw_in);
	chunkqueue_free(con->raw_out);

	g_slice_free1(con->srv->option_def_values->len * sizeof(option_value), con->options);

	vrequest_free(con->mainvr);
	http_request_parser_clear(&con->req_parser_ctx);

	if (con->keep_alive_data.link && con->wrk) {
		g_queue_delete_link(&con->wrk->keep_alive_queue, con->keep_alive_data.link);
		con->keep_alive_data.link = NULL;
	}
	con->keep_alive_data.timeout = 0;
	con->keep_alive_data.max_idle = 0;
	if (con->wrk)
		ev_timer_stop(con->wrk->loop, &con->keep_alive_data.watcher);

	g_slice_free(connection, con);
}

gchar *connection_state_str(connection_state_t state) {
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

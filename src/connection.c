
#include "base.h"
#include "network.h"
#include "utils.h"
#include "plugin_core.h"

/* only call it from the worker context the con belongs to */
void worker_con_put(connection *con); /* worker.c */

void internal_error(connection *con) {
	if (con->response_headers_sent) {
		CON_ERROR(con, "%s", "Couldn't send '500 Internal Error': headers already sent");
		connection_set_state(con, CON_STATE_ERROR);
	} else {
		http_headers_reset(con->response.headers);
		con->response.http_status = 500;
		con->content_handler = NULL;
		connection_set_state(con, CON_STATE_WRITE_RESPONSE);
	}
}

static void parse_request_body(connection *con) {
	if (     con->state >= CON_STATE_READ_REQUEST_CONTENT
	      && con->state <= CON_STATE_WRITE_RESPONSE
	      && !con->in->is_closed) {
		if (con->request.content_length == -1) {
			/* TODO: parse chunked encoded request body, filters */
			chunkqueue_steal_all(con->in, con->raw_in);
		} else {
			if (con->in->bytes_in < con->request.content_length) {
				chunkqueue_steal_len(con->in, con->raw_in, con->request.content_length - con->in->bytes_in);
			}
			if (con->in->bytes_in == con->request.content_length) con->in->is_closed = TRUE;
		}
	}
}

static void forward_response_body(connection *con) {
	if (con->state == CON_STATE_WRITE_RESPONSE && !con->raw_out->is_closed) {
		if (con->out->length > 0) {
			/* TODO: chunked encoding, filters */
			chunkqueue_steal_all(con->raw_out, con->out);
		}
		if (con->in->is_closed && 0 == con->raw_out->length)
			con->raw_out->is_closed = TRUE;
		if (con->raw_out->length > 0) {
			ev_io_add_events(con->wrk->loop, &con->sock_watcher, EV_WRITE);
		} else {
			ev_io_rem_events(con->wrk->loop, &con->sock_watcher, EV_WRITE);
		}
	}
}

static void connection_cb(struct ev_loop *loop, ev_io *w, int revents) {
	connection *con = (connection*) w->data;
	gboolean dojoblist = FALSE;
	UNUSED(loop);

	if (revents & EV_READ) {
		if (con->in->is_closed) {
			/* don't read the next request before current one is done */
			ev_io_rem_events(loop, w, EV_READ);
		} else {
			switch (network_read(con, w->fd, con->raw_in)) {
			case NETWORK_STATUS_SUCCESS:
				dojoblist = TRUE;
				break;
			case NETWORK_STATUS_FATAL_ERROR:
				CON_ERROR(con, "%s", "network read fatal error");
				connection_set_state(con, CON_STATE_ERROR);
				dojoblist = TRUE;
				break;
			case NETWORK_STATUS_CONNECTION_CLOSE:
				con->raw_in->is_closed = TRUE;
				shutdown(w->fd, SHUT_RD);
				connection_set_state(con, CON_STATE_CLOSE);
				dojoblist = TRUE;
				break;
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
			switch (network_write(con, w->fd, con->raw_out)) {
			case NETWORK_STATUS_SUCCESS:
				dojoblist = TRUE;
				break;
			case NETWORK_STATUS_FATAL_ERROR:
				CON_ERROR(con, "%s", "network write fatal error");
				connection_set_state(con, CON_STATE_ERROR);
				dojoblist = TRUE;
				break;
			case NETWORK_STATUS_CONNECTION_CLOSE:
				connection_set_state(con, CON_STATE_CLOSE);
				dojoblist = TRUE;
				break;
			case NETWORK_STATUS_WAIT_FOR_EVENT:
				break;
			case NETWORK_STATUS_WAIT_FOR_AIO_EVENT:
				/* TODO: aio */
				ev_io_rem_events(loop, w, EV_WRITE);
				break;
			case NETWORK_STATUS_WAIT_FOR_FD:
				/* TODO: wait for fd */
				ev_io_rem_events(loop, w, EV_WRITE);
				break;
			}
		}
		if (con->raw_out->length == 0) {
			ev_io_rem_events(loop, w, EV_WRITE);
			dojoblist = TRUE;
		}
	}

	if (dojoblist)
		joblist_append(con);
}

static void connection_keepalive_cb(struct ev_loop *loop, ev_timer *w, int revents) {
	connection *con = (connection*) w->data;
	UNUSED(loop); UNUSED(revents);
	worker_con_put(con);
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
	con->remote_addr_str = g_string_sized_new(0);
	con->local_addr_str = g_string_sized_new(0);
	con->keep_alive = TRUE;

	con->raw_in  = chunkqueue_new();
	con->raw_out = chunkqueue_new();
	con->in      = chunkqueue_new();
	con->out     = chunkqueue_new();

	action_stack_init(&con->action_stack);

	con->options = g_slice_copy(srv->option_count * sizeof(*srv->option_def_values), srv->option_def_values);

	request_init(&con->request);
	physical_init(&con->physical);
	response_init(&con->response);
	http_request_parser_init(&con->req_parser_ctx, &con->request, con->raw_in);

	con->keep_alive_data.link = NULL;
	con->keep_alive_data.timeout = 0;
	con->keep_alive_data.max_idle = 0;
	ev_init(&con->keep_alive_data.watcher, connection_keepalive_cb);
	con->keep_alive_data.watcher.data = con;

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
	chunkqueue_reset(con->in);
	chunkqueue_reset(con->out);

	action_stack_reset(con->srv, &con->action_stack);

	memcpy(con->options, con->srv->option_def_values, con->srv->option_count * sizeof(*con->srv->option_def_values));

	request_reset(&con->request);
	physical_reset(&con->physical);
	response_reset(&con->response);
	http_request_parser_reset(&con->req_parser_ctx);

	if (con->keep_alive_data.link) {
		g_queue_delete_link(&con->wrk->keep_alive_queue, con->keep_alive_data.link);
		con->keep_alive_data.link = NULL;
	}
	con->keep_alive_data.timeout = 0;
	con->keep_alive_data.max_idle = 0;
	ev_timer_stop(con->wrk->loop, &con->keep_alive_data.watcher);
}

void server_check_keepalive(server *srv);
void connection_reset_keep_alive(connection *con) {
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
	g_string_truncate(con->remote_addr_str, 0);
	g_string_truncate(con->local_addr_str, 0);
	con->keep_alive = TRUE;

	con->raw_out->is_closed = FALSE;
	chunkqueue_reset(con->in);
	chunkqueue_reset(con->out);

	action_stack_reset(con->srv, &con->action_stack);

	memcpy(con->options, con->srv->option_def_values, con->srv->option_count * sizeof(*con->srv->option_def_values));

	request_reset(&con->request);
	physical_reset(&con->physical);
	response_reset(&con->response);
	http_request_parser_reset(&con->req_parser_ctx);
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
	chunkqueue_free(con->in);
	chunkqueue_free(con->out);

	action_stack_clear(con->srv, &con->action_stack);

	g_slice_free1(con->srv->option_count * sizeof(*con->srv->option_def_values), con->options);

	request_clear(&con->request);
	physical_clear(&con->physical);
	response_clear(&con->response);
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

void connection_set_state(connection *con, connection_state_t state) {
	if (state < con->state) {
		CON_ERROR(con, "Cannot move into requested state: %i => %i, move to error state", con->state, state);
		state = CON_STATE_ERROR;
	}
	con->state = state;
}

void connection_state_machine(connection *con) {
	gboolean done = FALSE;
	do {
		switch (con->state) {
		case CON_STATE_DEAD:
			done = TRUE;
			break;

		case CON_STATE_KEEP_ALIVE:
			if (con->raw_in->length > 0) {
				/* stop keep alive timeout watchers */
				if (con->keep_alive_data.link) {
					g_queue_delete_link(&con->wrk->keep_alive_queue, con->keep_alive_data.link);
					con->keep_alive_data.link = NULL;
				}
				con->keep_alive_data.timeout = 0;
				ev_timer_stop(con->wrk->loop, &con->keep_alive_data.watcher);

				connection_set_state(con, CON_STATE_REQUEST_START);
			} else
				done = TRUE;
			break;


		case CON_STATE_REQUEST_START:
			connection_set_state(con, CON_STATE_READ_REQUEST_HEADER);
			action_enter(con, con->srv->mainaction);
			break;

		case CON_STATE_READ_REQUEST_HEADER:
			if (CORE_OPTION(CORE_OPTION_DEBUG_REQUEST_HANDLING).boolean) {
				CON_TRACE(con, "%s", "reading request header");
			}
			switch(http_request_parse(con, &con->req_parser_ctx)) {
			case HANDLER_FINISHED:
			case HANDLER_GO_ON:
				connection_set_state(con, CON_STATE_VALIDATE_REQUEST_HEADER);
				break;
			case HANDLER_WAIT_FOR_FD:
				/* TODO: wait for fd */
				done = TRUE;
				break;
			case HANDLER_WAIT_FOR_EVENT:
				done = TRUE;
				break;
			case HANDLER_ERROR:
			case HANDLER_COMEBACK: /* unexpected */
				/* unparsable header */
				connection_set_state(con, CON_STATE_ERROR);
				break;
			}
			break;

		case CON_STATE_VALIDATE_REQUEST_HEADER:
			if (CORE_OPTION(CORE_OPTION_DEBUG_REQUEST_HANDLING).boolean) {
				CON_TRACE(con, "%s", "validating request header");
			}
			connection_set_state(con, CON_STATE_HANDLE_REQUEST_HEADER);
			request_validate_header(con);
			con->wrk->stats.requests++;
			break;

		case CON_STATE_HANDLE_REQUEST_HEADER:
			if (CORE_OPTION(CORE_OPTION_DEBUG_REQUEST_HANDLING).boolean) {
				CON_TRACE(con, "%s", "handle request header");
			}
			switch (action_execute(con)) {
			case ACTION_WAIT_FOR_EVENT:
				done = TRUE;
				break;
			case ACTION_GO_ON:
			case ACTION_FINISHED:
				if (con->state == CON_STATE_HANDLE_REQUEST_HEADER) {
					internal_error(con);
				}
				connection_set_state(con, CON_STATE_WRITE_RESPONSE);
				break;
			case ACTION_ERROR:
				internal_error(con);
				break;
			}
			break;

		case CON_STATE_READ_REQUEST_CONTENT:
		case CON_STATE_HANDLE_RESPONSE_HEADER:
			if (CORE_OPTION(CORE_OPTION_DEBUG_REQUEST_HANDLING).boolean) {
				CON_TRACE(con, "%s", "read request/handle response header");
			}
			if (con->expect_100_cont) {
				if (CORE_OPTION(CORE_OPTION_DEBUG_REQUEST_HANDLING).boolean) {
					CON_TRACE(con, "%s", "send 100 Continue");
				}
				chunkqueue_append_mem(con->raw_out, CONST_STR_LEN("HTTP/1.1 100 Continue\r\n\r\n"));
				con->expect_100_cont = FALSE;
				ev_io_add_events(con->wrk->loop, &con->sock_watcher, EV_WRITE);
			}
			parse_request_body(con);

			if (con->content_handler)
				con->content_handler->handle_content(con, con->content_handler);

			switch (action_execute(con)) {
			case ACTION_WAIT_FOR_EVENT:
				done = TRUE;
				break;
			case ACTION_GO_ON:
			case ACTION_FINISHED:
				connection_set_state(con, CON_STATE_WRITE_RESPONSE);
				break;
			case ACTION_ERROR:
				internal_error(con);
				break;
			}
			break;

		case CON_STATE_WRITE_RESPONSE:
			if (con->in->is_closed && con->raw_out->is_closed) {
				connection_set_state(con, CON_STATE_RESPONSE_END);
				break;
			}

			if (!con->response_headers_sent) {
				con->response_headers_sent = TRUE;
				if (CORE_OPTION(CORE_OPTION_DEBUG_REQUEST_HANDLING).boolean) {
					CON_TRACE(con, "%s", "write response headers");
				}
				response_send_headers(con);
			}

			if (CORE_OPTION(CORE_OPTION_DEBUG_REQUEST_HANDLING).boolean) {
				CON_TRACE(con, "%s", "write response");
			}

			parse_request_body(con);

			if (con->content_handler)
				con->content_handler->handle_content(con, con->content_handler);

			forward_response_body(con);

			if (con->in->is_closed && con->raw_out->is_closed) {
				connection_set_state(con, CON_STATE_RESPONSE_END);
				break;
			}
			if (con->state == CON_STATE_WRITE_RESPONSE) done = TRUE;
			break;

		case CON_STATE_RESPONSE_END:
			if (CORE_OPTION(CORE_OPTION_DEBUG_REQUEST_HANDLING).boolean) {
				CON_TRACE(con, "response end (keep_alive = %i)", con->keep_alive);
			}

			plugins_handle_close(con);

			if (con->keep_alive) {
				connection_reset_keep_alive(con);
			} else {
				worker_con_put(con);
				done = TRUE;
			}
			break;

		case CON_STATE_CLOSE:
			if (CORE_OPTION(CORE_OPTION_DEBUG_REQUEST_HANDLING).boolean) {
				CON_TRACE(con, "%s", "connection closed");
			}

			plugins_handle_close(con);

			worker_con_put(con);
			done = TRUE;
			break;

		case CON_STATE_ERROR:
			if (CORE_OPTION(CORE_OPTION_DEBUG_REQUEST_HANDLING).boolean) {
				CON_TRACE(con, "%s", "connection closed (error)");
			}

			plugins_handle_close(con);

			worker_con_put(con);
			done = TRUE;
			break;
		}
	} while (!done);
}

void connection_handle_direct(connection *con) {
	connection_set_state(con, CON_STATE_WRITE_RESPONSE);
	con->out->is_closed = TRUE;
}

void connection_handle_indirect(connection *con, plugin *p) {
	if (!p) {
		connection_handle_direct(con);
	} else if (p->handle_content) {
		connection_set_state(con, CON_STATE_READ_REQUEST_CONTENT);
		con->content_handler = p;
	} else {
		CON_ERROR(con, "Indirect plugin '%s' handler has no handle_content callback", p->name);
		internal_error(con);
	}
}

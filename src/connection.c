
#include "connection.h"
#include "network.h"
#include "utils.h"
#include "plugin_core.h"

void con_put(server *srv, connection *con); /* server.c */

void internal_error(server *srv, connection *con) {
	if (con->response_headers_sent) {
		CON_ERROR(srv, con, "%s", "Couldn't send '500 Internal Error': headers already sent");
		connection_set_state(srv, con, CON_STATE_ERROR);
	} else {
		http_headers_reset(con->response.headers);
		con->response.http_status = 500;
		con->content_handler = NULL;
		connection_set_state(srv, con, CON_STATE_WRITE_RESPONSE);
	}
}

static void parse_request_body(server *srv, connection *con) {
	UNUSED(srv);
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

static void forward_response_body(server *srv, connection *con) {
	UNUSED(srv);
	if (con->state == CON_STATE_WRITE_RESPONSE && !con->raw_out->is_closed) {
		if (con->out->length > 0) {
			/* TODO: chunked encoding, filters */
			chunkqueue_steal_all(con->raw_out, con->out);
		}
		if (con->in->is_closed && 0 == con->raw_out->length)
			con->raw_out->is_closed = TRUE;
		if (con->raw_out->length > 0) {
			ev_io_add_events(srv->loop, &con->sock.watcher, EV_WRITE);
		} else {
			ev_io_rem_events(srv->loop, &con->sock.watcher, EV_WRITE);
		}
	}
}

static void connection_cb(struct ev_loop *loop, ev_io *w, int revents) {
	connection_socket *con_sock = (connection_socket*) w->data;
	server *srv = con_sock->srv;
	connection *con = con_sock->con;
	gboolean dojoblist = FALSE;
	UNUSED(loop);

	if (revents & EV_READ) {
		if (con->in->is_closed) {
			/* don't read the next request before current one is done */
			ev_io_rem_events(loop, w, EV_READ);
		} else {
			switch (network_read(srv, con, w->fd, con->raw_in)) {
			case NETWORK_STATUS_SUCCESS:
				dojoblist = TRUE;
				break;
			case NETWORK_STATUS_FATAL_ERROR:
				CON_ERROR(srv, con, "%s", "network read fatal error");
				connection_set_state(srv, con, CON_STATE_ERROR);
				dojoblist = TRUE;
				break;
			case NETWORK_STATUS_CONNECTION_CLOSE:
				con->raw_in->is_closed = TRUE;
				shutdown(w->fd, SHUT_RD);
				connection_set_state(srv, con, CON_STATE_CLOSE);
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
			switch (network_write(srv, con, w->fd, con->raw_out)) {
			case NETWORK_STATUS_SUCCESS:
				dojoblist = TRUE;
				break;
			case NETWORK_STATUS_FATAL_ERROR:
				CON_ERROR(srv, con, "%s", "network write fatal error");
				connection_set_state(srv, con, CON_STATE_ERROR);
				dojoblist = TRUE;
				break;
			case NETWORK_STATUS_CONNECTION_CLOSE:
				connection_set_state(srv, con, CON_STATE_CLOSE);
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
		joblist_append(srv, con);
}

connection* connection_new(server *srv) {
	connection *con = g_slice_new0(connection);
	UNUSED(srv);

	con->state = CON_STATE_REQUEST_START;
	con->response_headers_sent = FALSE;
	con->expect_100_cont = FALSE;

	my_ev_init(&con->sock.watcher, connection_cb);
	ev_io_set(&con->sock.watcher, -1, 0);
	con->sock.srv = srv; con->sock.con = con; con->sock.watcher.data = &con->sock;
	con->remote_addr_str = g_string_sized_new(0);
	con->local_addr_str = g_string_sized_new(0);
	con->keep_alive = TRUE;

	con->raw_in  = chunkqueue_new();
	con->raw_out = chunkqueue_new();
	con->in      = chunkqueue_new();
	con->out     = chunkqueue_new();

	action_stack_init(&con->action_stack);

	con->options = g_slice_copy(srv->option_count * sizeof(*srv->option_def_values), srv->option_def_values);

	request_init(&con->request, con->raw_in);
	physical_init(&con->physical);
	response_init(&con->response);

	return con;
}

void connection_reset(server *srv, connection *con) {
	con->state = CON_STATE_REQUEST_START;
	con->response_headers_sent = FALSE;
	con->expect_100_cont = FALSE;

	ev_io_stop(srv->loop, &con->sock.watcher);
	if (con->sock.watcher.fd != -1) {
		if (con->raw_in->is_closed) { /* read already shutdown */
			shutdown(con->sock.watcher.fd, SHUT_WR);
			close(con->sock.watcher.fd);
		} else {
			server_add_closing_socket(srv, con->sock.watcher.fd);
		}
	}
	ev_io_set(&con->sock.watcher, -1, 0);
	g_string_truncate(con->remote_addr_str, 0);
	g_string_truncate(con->local_addr_str, 0);
	con->keep_alive = TRUE;

	chunkqueue_reset(con->raw_in);
	chunkqueue_reset(con->raw_out);
	chunkqueue_reset(con->in);
	chunkqueue_reset(con->out);

	action_stack_reset(srv, &con->action_stack);

	memcpy(con->options, srv->option_def_values, srv->option_count * sizeof(*srv->option_def_values));

	request_reset(&con->request);
	physical_reset(&con->physical);
	response_reset(&con->response);
}

void connection_reset_keep_alive(server *srv, connection *con) {
	con->state = CON_STATE_REQUEST_START;
	con->response_headers_sent = FALSE;
	con->expect_100_cont = FALSE;

	ev_io_set_events(srv->loop, &con->sock.watcher, EV_READ);
	g_string_truncate(con->remote_addr_str, 0);
	g_string_truncate(con->local_addr_str, 0);
	con->keep_alive = TRUE;

	con->raw_out->is_closed = FALSE;
	chunkqueue_reset(con->in);
	chunkqueue_reset(con->out);

	action_stack_reset(srv, &con->action_stack);

	memcpy(con->options, srv->option_def_values, srv->option_count * sizeof(*srv->option_def_values));

	request_reset(&con->request);
	physical_reset(&con->physical);
	response_reset(&con->response);
}

void connection_free(server *srv, connection *con) {
	con->state = CON_STATE_REQUEST_START;
	con->response_headers_sent = FALSE;
	con->expect_100_cont = FALSE;

	ev_io_stop(srv->loop, &con->sock.watcher);
	if (con->sock.watcher.fd != -1) {
		if (con->raw_in->is_closed) { /* read already shutdown */
			shutdown(con->sock.watcher.fd, SHUT_WR);
			close(con->sock.watcher.fd);
		} else {
			server_add_closing_socket(srv, con->sock.watcher.fd);
		}
	}
	ev_io_set(&con->sock.watcher, -1, 0);
	g_string_free(con->remote_addr_str, TRUE);
	g_string_free(con->local_addr_str, TRUE);
	con->keep_alive = TRUE;

	chunkqueue_free(con->raw_in);
	chunkqueue_free(con->raw_out);
	chunkqueue_free(con->in);
	chunkqueue_free(con->out);

	action_stack_clear(srv, &con->action_stack);

	g_slice_free1(srv->option_count * sizeof(*srv->option_def_values), con->options);

	request_clear(&con->request);
	physical_clear(&con->physical);
	response_clear(&con->response);

	g_slice_free(connection, con);
}

void connection_set_state(server *srv, connection *con, connection_state_t state) {
	if (state < con->state) {
		CON_ERROR(srv, con, "Cannot move into requested state: %i => %i, move to error state", con->state, state);
		state = CON_STATE_ERROR;
	}
	con->state = state;
}

void connection_state_machine(server *srv, connection *con) {
	gboolean done = FALSE;
	do {
		switch (con->state) {
		case CON_STATE_REQUEST_START:
			connection_set_state(srv, con, CON_STATE_READ_REQUEST_HEADER);
			action_enter(con, srv->mainaction);
			break;

		case CON_STATE_READ_REQUEST_HEADER:
			if (CORE_OPTION(CORE_OPTION_DEBUG_REQUEST_HANDLING)) {
				TRACE(srv, "%s", "reading request header");
			}
			switch(http_request_parse(srv, con, &con->request.parser_ctx)) {
			case HANDLER_FINISHED:
			case HANDLER_GO_ON:
				connection_set_state(srv, con, CON_STATE_VALIDATE_REQUEST_HEADER);
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
				connection_set_state(srv, con, CON_STATE_ERROR);
				break;
			}
			break;

		case CON_STATE_VALIDATE_REQUEST_HEADER:
			if (CORE_OPTION(CORE_OPTION_DEBUG_REQUEST_HANDLING)) {
				TRACE(srv, "%s", "validating request header");
			}
			connection_set_state(srv, con, CON_STATE_HANDLE_REQUEST_HEADER);
			request_validate_header(srv, con);
			srv->stats.requests++;
			break;

		case CON_STATE_HANDLE_REQUEST_HEADER:
			if (CORE_OPTION(CORE_OPTION_DEBUG_REQUEST_HANDLING)) {
				TRACE(srv, "%s", "handle request header");
			}
			switch (action_execute(srv, con)) {
			case ACTION_WAIT_FOR_EVENT:
				done = TRUE;
				break;
			case ACTION_GO_ON:
			case ACTION_FINISHED:
				if (con->state == CON_STATE_HANDLE_REQUEST_HEADER) {
					internal_error(srv, con);
				}
				connection_set_state(srv, con, CON_STATE_WRITE_RESPONSE);
				break;
			case ACTION_ERROR:
				internal_error(srv, con);
				break;
			}
			break;

		case CON_STATE_READ_REQUEST_CONTENT:
		case CON_STATE_HANDLE_RESPONSE_HEADER:
			if (CORE_OPTION(CORE_OPTION_DEBUG_REQUEST_HANDLING)) {
				TRACE(srv, "%s", "read request/handle response header");
			}
			if (con->expect_100_cont) {
				if (CORE_OPTION(CORE_OPTION_DEBUG_REQUEST_HANDLING)) {
					TRACE(srv, "%s", "send 100 Continue");
				}
				chunkqueue_append_mem(con->raw_out, CONST_STR_LEN("HTTP/1.1 100 Continue\r\n\r\n"));
				con->expect_100_cont = FALSE;
				ev_io_add_events(srv->loop, &con->sock.watcher, EV_WRITE);
			}
			parse_request_body(srv, con);

			if (con->content_handler)
				con->content_handler->handle_content(srv, con, con->content_handler);

			switch (action_execute(srv, con)) {
			case ACTION_WAIT_FOR_EVENT:
				done = TRUE;
				break;
			case ACTION_GO_ON:
			case ACTION_FINISHED:
				connection_set_state(srv, con, CON_STATE_WRITE_RESPONSE);
				break;
			case ACTION_ERROR:
				internal_error(srv, con);
				break;
			}
			break;

		case CON_STATE_WRITE_RESPONSE:
			if (con->in->is_closed && con->raw_out->is_closed) {
				connection_set_state(srv, con, CON_STATE_RESPONSE_END);
				break;
			}

			if (!con->response_headers_sent) {
				con->response_headers_sent = TRUE;
				if (CORE_OPTION(CORE_OPTION_DEBUG_REQUEST_HANDLING)) {
					TRACE(srv, "%s", "write response headers");
				}
				response_send_headers(srv, con);
			}

			if (CORE_OPTION(CORE_OPTION_DEBUG_REQUEST_HANDLING)) {
				TRACE(srv, "%s", "write response");
			}

			parse_request_body(srv, con);

			if (con->content_handler)
				con->content_handler->handle_content(srv, con, con->content_handler);

			forward_response_body(srv, con);

			if (con->in->is_closed && con->raw_out->is_closed) {
				connection_set_state(srv, con, CON_STATE_RESPONSE_END);
				break;
			}
			if (con->state == CON_STATE_WRITE_RESPONSE) done = TRUE;
			break;

		case CON_STATE_RESPONSE_END:
			if (CORE_OPTION(CORE_OPTION_DEBUG_REQUEST_HANDLING)) {
				TRACE(srv, "response end (keep_alive = %i)", con->keep_alive);
			}

			plugins_handle_close(srv, con);

			if (con->keep_alive) {
				connection_reset_keep_alive(srv, con);
			} else {
				con_put(srv, con);
				done = TRUE;
			}
			break;

		case CON_STATE_CLOSE:
			if (CORE_OPTION(CORE_OPTION_DEBUG_REQUEST_HANDLING)) {
				TRACE(srv, "%s", "connection closed");
			}

			plugins_handle_close(srv, con);

			con_put(srv, con);
			done = TRUE;
			break;

		case CON_STATE_ERROR:
			if (CORE_OPTION(CORE_OPTION_DEBUG_REQUEST_HANDLING)) {
				TRACE(srv, "%s", "connection closed (error)");
			}

			plugins_handle_close(srv, con);

			con_put(srv, con);
			done = TRUE;
			break;
		}
	} while (!done);
}

void connection_handle_direct(server *srv, connection *con) {
	connection_set_state(srv, con, CON_STATE_WRITE_RESPONSE);
	con->out->is_closed = TRUE;
}

void connection_handle_indirect(server *srv, connection *con, plugin *p) {
	if (!p) {
		connection_handle_direct(srv, con);
	} else if (p->handle_content) {
		connection_set_state(srv, con, CON_STATE_READ_REQUEST_CONTENT);
		con->content_handler = p;
	} else {
		CON_ERROR(srv, con, "Indirect plugin '%s' handler has no handle_content callback", p->name);
		internal_error(srv, con);
	}
}

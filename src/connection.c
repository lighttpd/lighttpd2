
#include "connection.h"
#include "network.h"
#include "utils.h"

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
			ev_io_set(w, w->fd, w->events && ~EV_READ);
		} else {
			switch (network_read(srv, con, w->fd, con->raw_in)) {
			case NETWORK_STATUS_SUCCESS:
				dojoblist = TRUE;
				break;
			case NETWORK_STATUS_FATAL_ERROR:
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
				/* TODO ? */
				ev_io_rem_events(loop, w, EV_READ);
				break;
			case NETWORK_STATUS_WAIT_FOR_FD:
				/* TODO */
				ev_io_rem_events(loop, w, EV_READ);
				break;
			}
		}
	}

	if (revents & EV_WRITE) {
		if (con->raw_out->length > 0) {
			network_write(srv, con, w->fd, con->raw_out);
// 			CON_TRACE(srv, con, "cq->len: raw_out=%i, out=%i", (int) con->raw_out->length, (int) con->out->length);
			dojoblist = TRUE;
		}
		if (con->raw_out->length == 0) {
// 			CON_TRACE(srv, con, "%s", "stop write");
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

	con->raw_in  = chunkqueue_new();
	con->raw_out = chunkqueue_new();
	con->in      = chunkqueue_new();
	con->out     = chunkqueue_new();

	ev_io_init(&con->sock.watcher, connection_cb, -1, 0);
	con->sock.srv = srv; con->sock.con = con; con->sock.watcher.data = &con->sock;
	con->remote_addr_str = g_string_sized_new(0);
	con->local_addr_str = g_string_sized_new(0);
	con->keep_alive = TRUE;

	action_stack_init(&con->action_stack);

	request_init(&con->request, con->raw_in);
	response_init(&con->response);

	return con;
}

void connection_reset(server *srv, connection *con) {
	con->state = CON_STATE_REQUEST_START;
	con->response_headers_sent = FALSE;
	con->expect_100_cont = FALSE;

	chunkqueue_reset(con->raw_in);
	chunkqueue_reset(con->raw_out);
	chunkqueue_reset(con->in);
	chunkqueue_reset(con->out);

	ev_io_stop(srv->loop, &con->sock.watcher);
	if (con->sock.watcher.fd != -1) {
		shutdown(con->sock.watcher.fd, SHUT_RDWR);
		close(con->sock.watcher.fd);
	}
	ev_io_set(&con->sock.watcher, -1, 0);
	g_string_truncate(con->remote_addr_str, 0);
	g_string_truncate(con->local_addr_str, 0);
	con->keep_alive = TRUE;

	action_stack_reset(srv, &con->action_stack);

	request_reset(&con->request);
	response_reset(&con->response);
}

void connection_reset_keep_alive(server *srv, connection *con) {
	con->state = CON_STATE_REQUEST_START;
	con->response_headers_sent = FALSE;
	con->expect_100_cont = FALSE;

	con->raw_out->is_closed = FALSE;
	chunkqueue_reset(con->in);
	chunkqueue_reset(con->out);

	ev_io_set_events(srv->loop, &con->sock.watcher, EV_READ);
	g_string_truncate(con->remote_addr_str, 0);
	g_string_truncate(con->local_addr_str, 0);
	con->keep_alive = TRUE;

	action_stack_reset(srv, &con->action_stack);

	request_reset(&con->request);
	response_reset(&con->response);
}

void connection_free(server *srv, connection *con) {
	con->state = CON_STATE_REQUEST_START;
	con->response_headers_sent = FALSE;
	con->expect_100_cont = FALSE;

	chunkqueue_free(con->raw_in);
	chunkqueue_free(con->raw_out);
	chunkqueue_free(con->in);
	chunkqueue_free(con->out);

	ev_io_stop(srv->loop, &con->sock.watcher);
	if (con->sock.watcher.fd != -1) {
		shutdown(con->sock.watcher.fd, SHUT_RDWR);
		close(con->sock.watcher.fd);
	}
	ev_io_set(&con->sock.watcher, -1, 0);
	g_string_free(con->remote_addr_str, TRUE);
	g_string_free(con->local_addr_str, TRUE);
	con->keep_alive = TRUE;

	action_stack_clear(srv, &con->action_stack);

	request_clear(&con->request);
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
			/* TODO: reset some values after keep alive - or do it in CON_STATE_REQUEST_END */
			connection_set_state(srv, con, CON_STATE_READ_REQUEST_HEADER);
			action_enter(con, srv->mainaction);
			break;
		case CON_STATE_READ_REQUEST_HEADER:
// 			TRACE(srv, "%s", "reading request header");
			switch(http_request_parse(srv, con, &con->request.parser_ctx)) {
			case HANDLER_FINISHED:
			case HANDLER_GO_ON:
				connection_set_state(srv, con, CON_STATE_VALIDATE_REQUEST_HEADER);
				break;
			case HANDLER_WAIT_FOR_FD:
				/* TODO */
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
// 			TRACE(srv, "%s", "validating request header");
			connection_set_state(srv, con, CON_STATE_HANDLE_REQUEST_HEADER);
			request_validate_header(srv, con);
			break;
		case CON_STATE_HANDLE_REQUEST_HEADER:
// 			TRACE(srv, "%s", "handle request header");
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
				/* action return error */
				/* TODO: return 500 instead ? */
				connection_set_state(srv, con, CON_STATE_ERROR);
				break;
			}
			break;
		case CON_STATE_READ_REQUEST_CONTENT:
		case CON_STATE_HANDLE_RESPONSE_HEADER:
// 			TRACE(srv, "%s", "read request/handle response header");
			parse_request_body(srv, con);
			/* TODO: call plugin content_handler */
			switch (action_execute(srv, con)) {
			case ACTION_WAIT_FOR_EVENT:
				done = TRUE;
				break;
			case ACTION_GO_ON:
			case ACTION_FINISHED:
				connection_set_state(srv, con, CON_STATE_WRITE_RESPONSE);
				break;
			case ACTION_ERROR:
				/* action return error */
				/* TODO: return 500 instead ? */
				connection_set_state(srv, con, CON_STATE_ERROR);
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
// 				TRACE(srv, "%s", "write response headers");
				response_send_headers(srv, con);
			}
// 			TRACE(srv, "%s", "write response");
			parse_request_body(srv, con);
			/* TODO: call plugin content_handler */
			forward_response_body(srv, con);

			if (con->in->is_closed && con->raw_out->is_closed) {
				connection_set_state(srv, con, CON_STATE_RESPONSE_END);
				break;
			}
			if (con->state == CON_STATE_WRITE_RESPONSE) done = TRUE;
			break;
		case CON_STATE_RESPONSE_END:
// 			TRACE(srv, "%s", "response end");
			/* TODO: call plugin callbacks */
			if (con->keep_alive) {
				connection_reset_keep_alive(srv, con);
			} else {
				con_put(srv, con);
				done = TRUE;
			}
			break;
		case CON_STATE_CLOSE:
// 			TRACE(srv, "%s", "connection closed");
			/* TODO: call plugin callbacks */
			con_put(srv, con);
			done = TRUE;
			break;
		case CON_STATE_ERROR:
// 			TRACE(srv, "%s", "connection closed (error)");
			/* TODO: call plugin callbacks */
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
	connection_set_state(srv, con, CON_STATE_READ_REQUEST_CONTENT);
	con->content_handler = p;
}

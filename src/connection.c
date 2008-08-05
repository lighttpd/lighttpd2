
#include "connection.h"
#include "network.h"
#include "log.h"

static void parse_request_body(server *srv, connection *con) {
	if (con->state == CON_STATE_HANDLE_RESPONSE && !con->in->is_closed) {
		/* TODO: parse chunked encoded request body */
		if (con->in->bytes_in < con->request.content_length) {
			chunkqueue_steal_len(con->in, con->raw_in, con->request.content_length - con->in->bytes_in);
			if (con->in->bytes_in == con->request.content_length) con->in->is_closed = TRUE;
		} else if (con->request.content_length == -1) {
			chunkqueue_steal_all(con->in, con->raw_in);
		}
	}
}

static void connection_cb(struct ev_loop *loop, ev_io *w, int revents) {
	connection_socket *con_sock = (connection_socket*) w->data;
	server *srv = con_sock->srv;
	connection *con = con_sock->con;

	if (revents && EV_READ) {
		if (con->in->is_closed) {
			/* don't read the next request before current one is done */
			ev_io_set(w, w->fd, w->events && ~EV_READ);
		} else {
			switch(network_read(srv, con, w->fd, con->raw_in)) {
			case NETWORK_STATUS_SUCCESS:
				parse_request_body(srv, con);
				joblist_append(srv, con);
				break;
			case NETWORK_STATUS_FATAL_ERROR:
				connection_set_state(srv, con, CON_STATE_ERROR);
				joblist_append(srv, con);
				break;
			case NETWORK_STATUS_CONNECTION_CLOSE:
				connection_set_state(srv, con, CON_STATE_CLOSE);
				joblist_append(srv, con);
				break;
			case NETWORK_STATUS_WAIT_FOR_EVENT:
				break;
			case NETWORK_STATUS_WAIT_FOR_AIO_EVENT:
				/* TODO ? */
				ev_io_set(w, w->fd, w->events && ~EV_READ);
				break;
			case NETWORK_STATUS_WAIT_FOR_FD:
				/* TODO */
				break;
			}
		}
	}

	if (revents && EV_WRITE) {
		if (con->raw_out->length > 0) {
			network_write(srv, con, w->fd, con->raw_out);
			joblist_append(srv, con);
		}
		if (con->raw_out->length == 0) {
			ev_io_set(w, w->fd, w->events && ~EV_WRITE);
		}
	}
}

connection* connection_new(server *srv) {
	connection *con = g_slice_new0(connection);
	UNUSED(srv);

	con->raw_in  = chunkqueue_new();
	con->raw_out = chunkqueue_new();
	con->in      = chunkqueue_new();
	con->out     = chunkqueue_new();

	con->sock.srv = srv; con->sock.con = con; con->sock.watcher.data = con;
	ev_io_init(&con->sock.watcher, connection_cb, -1, 0);
	con->remote_addr_str = g_string_sized_new(0);
	con->local_addr_str = g_string_sized_new(0);

	action_stack_init(&con->action_stack);

	request_init(&con->request, con->raw_in);

	return con;
}

void connection_reset(server *srv, connection *con) {
	chunkqueue_reset(con->raw_in);
	chunkqueue_reset(con->raw_out);
	chunkqueue_reset(con->in);
	chunkqueue_reset(con->out);

	ev_io_stop(srv->loop, &con->sock.watcher);
	close(con->sock.watcher.fd);
	ev_io_set(&con->sock.watcher, -1, 0);
	g_string_truncate(con->remote_addr_str, 0);
	g_string_truncate(con->local_addr_str, 0);

	action_stack_reset(srv, &con->action_stack);

	request_reset(&con->request);
}

void connection_free(server *srv, connection *con) {
	chunkqueue_free(con->raw_in);
	chunkqueue_free(con->raw_out);
	chunkqueue_free(con->in);
	chunkqueue_free(con->out);

	ev_io_stop(srv->loop, &con->sock.watcher);
	close(con->sock.watcher.fd);
	ev_io_set(&con->sock.watcher, -1, 0);
	g_string_free(con->remote_addr_str, TRUE);
	g_string_free(con->local_addr_str, TRUE);

	action_stack_clear(srv, &con->action_stack);

	request_clear(&con->request);

	g_slice_free(connection, con);
}

void connection_set_state(server *srv, connection *con, connection_state_t state) {
}

void connection_state_machine(server *srv, connection *con) {
	
}

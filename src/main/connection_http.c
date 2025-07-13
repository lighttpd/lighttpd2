/* handle plain HTTP server sockets */

#include <lighttpd/base.h>
#include <lighttpd/throttle.h>

typedef struct simple_tcp_connection simple_tcp_connection;
struct simple_tcp_connection {
	liIOStream *sock_stream;
	liConnectionSimpleTcpState simple_tcp_state;
	liConnection *con;
};

static void simple_tcp_io_cb(liIOStream *stream, liIOStreamEvent event) {
	simple_tcp_connection *data = stream->data;
	LI_FORCE_ASSERT(NULL != data);
	LI_FORCE_ASSERT(NULL == data->con || data == data->con->con_sock.data);
	LI_FORCE_ASSERT(NULL == data->sock_stream || stream == data->sock_stream);

	li_connection_simple_tcp(&data->con, stream, &data->simple_tcp_state, event);

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
	if (NULL == data) return;

	data->con = NULL;
	con->con_sock.data = NULL;
	con->con_sock.callbacks = NULL;

	li_stream_simple_socket_close(data->sock_stream, aborted);
	li_iostream_safe_release(&data->sock_stream);
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

gboolean li_connection_http_new(liConnection *con, int fd) {
	simple_tcp_connection *data = g_slice_new0(simple_tcp_connection);
	data->sock_stream = li_iostream_new(con->wrk, fd, simple_tcp_io_cb, data);
	li_connection_simple_tcp_init(&data->simple_tcp_state);
	data->con = con;
	con->con_sock.data = data;
	con->con_sock.callbacks = &simple_tcp_cbs;
	con->con_sock.raw_out = &data->sock_stream->stream_out;
	li_stream_connect(&data->sock_stream->stream_in, &con->proxy_protocol_filter.stream);
	con->con_sock.raw_in = &con->proxy_protocol_filter.stream;
	li_stream_acquire(con->con_sock.raw_out);
	li_stream_acquire(con->con_sock.raw_in);

	return TRUE;
}

/* basic TCP (or unix domain) stream socket handling */

#include <lighttpd/base.h>

void li_connection_simple_tcp(liConnection **pcon, liIOStream *stream, liConnectionSimpleTcpState *state, liIOStreamEvent event) {
	liConnection *con;
	goffset transfer_in = 0, transfer_out = 0;

	transfer_in = (NULL != stream->stream_in.out) ? stream->stream_in.out->bytes_in : 0;
	transfer_out = (NULL != stream->stream_out.out) ? stream->stream_out.out->bytes_out : 0;

	li_stream_simple_socket_io_cb_with_buffer(stream, event, &state->read_buffer);

	/* li_stream_simple_socket_io_cb_with_buffer might lead to *pcon == NULL */
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

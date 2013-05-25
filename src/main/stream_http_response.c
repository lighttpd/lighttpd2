#include <lighttpd/stream_http_response.h>

typedef struct liStreamHttpResponse liStreamHttpResponse;

struct liStreamHttpResponse {
	liHttpResponseCtx parse_response_ctx;

	liStream stream;
	liVRequest *vr;
	gboolean response_headers_finished;
};

static void stream_http_response_data(liStreamHttpResponse* shr) {
	if (NULL == shr->stream.source) return;

	if (!shr->response_headers_finished) {
		switch (li_http_response_parse(shr->vr, &shr->parse_response_ctx)) {
		case LI_HANDLER_GO_ON:
			shr->response_headers_finished = TRUE;
			li_vrequest_indirect_headers_ready(shr->vr);
			break;
		case LI_HANDLER_ERROR:
			VR_ERROR(shr->vr, "%s", "Parsing response header failed");
			li_vrequest_error(shr->vr);
			return;
		case LI_HANDLER_WAIT_FOR_EVENT:
			if (shr->stream.source->out->is_closed) {
				VR_ERROR(shr->vr, "%s", "Parsing response header failed (eos)");
				li_vrequest_error(shr->vr);
			}
			return;
		default:
			return;
		}
	}

	li_chunkqueue_steal_all(shr->stream.out, shr->stream.source->out);
	if (shr->stream.source->out->is_closed) {
		shr->stream.out->is_closed = TRUE;
		li_stream_disconnect(&shr->stream);
	}
	li_stream_notify(&shr->stream);
}


static void stream_http_response_cb(liStream *stream, liStreamEvent event) {
	liStreamHttpResponse* shr = LI_CONTAINER_OF(stream, liStreamHttpResponse, stream);

	switch (event) {
	case LI_STREAM_NEW_DATA:
		stream_http_response_data(shr);
		break;
	case LI_STREAM_DISCONNECTED_DEST:
		shr->vr = NULL;
		li_stream_disconnect(stream);
		break;
	case LI_STREAM_DISCONNECTED_SOURCE:
		shr->vr = NULL;
		if (!stream->out->is_closed) {
			/* "abort" */
			li_stream_disconnect_dest(stream);
		}
		break;
	case LI_STREAM_DESTROY:
		li_http_response_parser_clear(&shr->parse_response_ctx);
		g_slice_free(liStreamHttpResponse, shr);
		break;
	default:
		break;
	}
}

LI_API liStream* li_stream_http_response_handle(liStream *http_in, liVRequest *vr, gboolean accept_cgi, gboolean accept_nph) {
	liStreamHttpResponse *shr = g_slice_new0(liStreamHttpResponse);
	shr->response_headers_finished = FALSE;
	shr->vr = vr;
	li_stream_init(&shr->stream, &vr->wrk->loop, stream_http_response_cb);
	li_http_response_parser_init(&shr->parse_response_ctx, &vr->response, http_in->out,
		accept_cgi, accept_nph);
	li_stream_connect(http_in, &shr->stream);
	return &shr->stream;
}

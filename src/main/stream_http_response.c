#include <lighttpd/stream_http_response.h>

typedef struct liStreamHttpResponse liStreamHttpResponse;

struct liStreamHttpResponse {
	liHttpResponseCtx parse_response_ctx;

	liStream stream;
	liVRequest *vr;
	gboolean response_headers_finished, transfer_encoding_chunked;
	liFilterChunkedDecodeState chunked_decode_state;
};

static gboolean check_response_header(liStreamHttpResponse* shr) {
	liResponse *resp = &shr->vr->response;
	liHttpHeader *hh;
	GList *l;

	shr->transfer_encoding_chunked = FALSE;

	/* Transfer-Encoding: chunked */
	l = li_http_header_find_first(resp->headers, CONST_STR_LEN("transfer-encoding"));
	if (l) {
		for ( ; l ; l = li_http_header_find_next(l, CONST_STR_LEN("transfer-encoding")) ) {
			hh = (liHttpHeader*) l->data;
			if (0 == g_ascii_strcasecmp( LI_HEADER_VALUE(hh), "identity" )) {
				/* ignore */
				continue;
			} if (0 == g_ascii_strcasecmp( LI_HEADER_VALUE(hh), "chunked" )) {
				if (shr->transfer_encoding_chunked) {
					VR_ERROR(shr->vr, "%s", "Response is chunked encoded twice");
					li_vrequest_error(shr->vr);
					return FALSE;
				}
				shr->transfer_encoding_chunked = TRUE;
			} else {
				VR_ERROR(shr->vr, "Response has unsupported Transfer-Encoding: %s", LI_HEADER_VALUE(hh));
				li_vrequest_error(shr->vr);
				return FALSE;
			}
		}
		li_http_header_remove(resp->headers, CONST_STR_LEN("transfer-encoding"));
		/* any non trivial transfer-encoding overwrites content-length */
		if (shr->transfer_encoding_chunked) {
			li_http_header_remove(resp->headers, CONST_STR_LEN("content-length"));
		}
	}

	shr->response_headers_finished = TRUE;
	li_vrequest_indirect_headers_ready(shr->vr);

	return TRUE;
}

static void stream_http_response_data(liStreamHttpResponse* shr) {
	if (NULL == shr->stream.source) return;

	if (!shr->response_headers_finished) {
		switch (li_http_response_parse(shr->vr, &shr->parse_response_ctx)) {
		case LI_HANDLER_GO_ON:
			if (!check_response_header(shr)) return;
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

	if (shr->transfer_encoding_chunked) {
		if (!li_filter_chunked_decode(shr->vr, shr->stream.out, shr->stream.source->out, &shr->chunked_decode_state)) {
			if (NULL != shr->vr) {
				VR_ERROR(shr->vr, "%s", "Decoding chunks failed");
				li_vrequest_error(shr->vr);
			} else {
				li_stream_reset(&shr->stream);
			}
		}
		if (shr->stream.source->out->is_closed) {
			li_stream_disconnect(&shr->stream);
		}
	} else {
		li_chunkqueue_steal_all(shr->stream.out, shr->stream.source->out);
		if (shr->stream.source->out->is_closed) {
			shr->stream.out->is_closed = TRUE;
			li_stream_disconnect(&shr->stream);
		}
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

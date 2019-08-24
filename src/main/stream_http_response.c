#include <lighttpd/stream_http_response.h>

typedef struct liStreamHttpResponse liStreamHttpResponse;

struct liStreamHttpResponse {
	liHttpResponseCtx parse_response_ctx;

	liStream stream;
	liVRequest *vr;
	gboolean keepalive, response_headers_finished, transfer_encoding_chunked, wait_for_close;
	goffset content_length;
	liFilterChunkedDecodeState chunked_decode_state;
};

static void check_response_header(liStreamHttpResponse* shr) {
	liResponse *resp = &shr->vr->response;
	GList *l;

	shr->transfer_encoding_chunked = FALSE;
	/* if protocol doesn't support keep-alive just wait for stream end */
	shr->wait_for_close = !shr->keepalive;
	shr->content_length = -1;

	/* Transfer-Encoding: chunked */
	l = li_http_header_find_first(resp->headers, CONST_STR_LEN("transfer-encoding"));
	if (l) {
		for ( ; l ; l = li_http_header_find_next(l, CONST_STR_LEN("transfer-encoding")) ) {
			liHttpHeader *hh = (liHttpHeader*) l->data;
			if (0 == g_ascii_strcasecmp( LI_HEADER_VALUE(hh), "identity" )) {
				/* ignore */
				continue;
			} if (0 == g_ascii_strcasecmp( LI_HEADER_VALUE(hh), "chunked" )) {
				if (shr->transfer_encoding_chunked) {
					VR_ERROR(shr->vr, "%s", "Response is chunked encoded twice");
					li_vrequest_error(shr->vr);
					return;
				}
				shr->transfer_encoding_chunked = TRUE;
			} else {
				VR_ERROR(shr->vr, "Response has unsupported Transfer-Encoding: %s", LI_HEADER_VALUE(hh));
				li_vrequest_error(shr->vr);
				return;
			}
		}
		li_http_header_remove(resp->headers, CONST_STR_LEN("transfer-encoding"));
		/* any non trivial transfer-encoding overwrites content-length */
		if (shr->transfer_encoding_chunked) {
			li_http_header_remove(resp->headers, CONST_STR_LEN("content-length"));
		}
	}

	/* Upgrade: */
	l = li_http_header_find_first(resp->headers, CONST_STR_LEN("upgrade"));
	if (l) {
		gboolean have_connection_upgrade = FALSE;
		liHttpHeaderTokenizer header_tokenizer;
		GString *token;
		if (101 != resp->http_status) {
			VR_ERROR(shr->vr, "Upgrade but status is %i instead of 101 'Switching Protocols'", resp->http_status);
			li_vrequest_error(shr->vr);
			return;
		}
		if (shr->transfer_encoding_chunked) {
			VR_ERROR(shr->vr, "%s", "Upgrade with Transfer-Encoding: chunked");
			li_vrequest_error(shr->vr);
			return;
		}
		/* requires Connection: Upgrade header */
		token = g_string_sized_new(15);
		li_http_header_tokenizer_start(&header_tokenizer, resp->headers, CONST_STR_LEN("Connection"));
		while (li_http_header_tokenizer_next(&header_tokenizer, token)) {
			VR_DEBUG(shr->vr, "Parsing header '%s'", ((liHttpHeader*)header_tokenizer.cur->data)->data->str);
			VR_DEBUG(shr->vr, "Connection token '%s'", token->str);
			if (0 == g_ascii_strcasecmp(token->str, "Upgrade")) {
				have_connection_upgrade = TRUE;
				break;
			}
		}
		g_string_free(token, TRUE); token = NULL;
		if (!have_connection_upgrade) {
			VR_ERROR(shr->vr, "%s", "Upgrade without Connection: Upgrade Transfer");
			li_vrequest_error(shr->vr);
			return;
		}
		shr->response_headers_finished = TRUE;
		shr->vr->backend_drain->out->is_closed = FALSE;
		{
			/* li_vrequest_connection_upgrade releases vr->backend_drain; keep our own reference */
			liStream *backend_drain = shr->vr->backend_drain;
			shr->vr->backend_drain = NULL;
			li_vrequest_connection_upgrade(shr->vr, backend_drain, &shr->stream);
			li_stream_release(backend_drain);
		}
		return;
	}

	if (!shr->transfer_encoding_chunked && shr->keepalive) {
		/**
		 * if protocol has HTTP "keepalive" concept and encoding isn't chunked,
		 * we need to check for content-length or "connection: close" indications.
		 * otherwise we won't know when the response is done
		 */
		liHttpHeader *hh;

		switch (shr->parse_response_ctx.http_version) {
		case LI_HTTP_VERSION_1_0:
			if (!li_http_header_is(shr->vr->response.headers, CONST_STR_LEN("connection"), CONST_STR_LEN("keep-alive")))
				shr->wait_for_close = TRUE;
			break;
		case LI_HTTP_VERSION_1_1:
			if (li_http_header_is(shr->vr->response.headers, CONST_STR_LEN("connection"), CONST_STR_LEN("close")))
				shr->wait_for_close = TRUE;
			break;
		case LI_HTTP_VERSION_UNSET:
			break;
		}

		/* content-length */
		hh = li_http_header_lookup(shr->vr->response.headers, CONST_STR_LEN("content-length"));
		if (hh) {
			const gchar *val = LI_HEADER_VALUE(hh);
			gint64 r;
			char *err;

			r = g_ascii_strtoll(val, &err, 10);
			if (*err != '\0') {
				VR_ERROR(shr->vr, "Backend response: content-length is not a number: %s", err);
				li_vrequest_error(shr->vr);
				return;
			}

			/**
			 * negative content-length is not supported
			 * and is a bad request
			 */
			if (r < 0) {
				VR_ERROR(shr->vr, "%s", "Backend response: content-length is negative");
				li_vrequest_error(shr->vr);
				return;
			}

			/**
			 * check if we had a over- or underrun in the string conversion
			 */
			if (r == G_MININT64 || r == G_MAXINT64) {
				if (errno == ERANGE) {
					VR_ERROR(shr->vr, "%s", "Backend response: content-length overflow");
					li_vrequest_error(shr->vr);
					return;
				}
			}

			shr->content_length = r;
			shr->wait_for_close = FALSE;
		}

		if (!shr->wait_for_close && shr->content_length < 0) {
			VR_ERROR(shr->vr, "%s", "Backend: need chunked transfer-encoding or content-length for keepalive connections");
			li_vrequest_error(shr->vr);
			return;
		}
	}

	shr->response_headers_finished = TRUE;
	li_vrequest_indirect_headers_ready(shr->vr);

	return;
}

static void stream_http_response_data(liStreamHttpResponse* shr) {
	if (NULL == shr->stream.source) return;

	if (!shr->response_headers_finished) {
		switch (li_http_response_parse(shr->vr, &shr->parse_response_ctx)) {
		case LI_HANDLER_GO_ON:
			check_response_header(shr);
			if (NULL == shr->stream.source) return;
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
	} else if (shr->wait_for_close) {
		li_chunkqueue_steal_all(shr->stream.out, shr->stream.source->out);
		if (shr->stream.source->out->is_closed) {
			shr->stream.out->is_closed = TRUE;
			li_stream_disconnect(&shr->stream);
		}
	} else {
		g_assert(shr->content_length >= 0);
		if (shr->content_length > 0) {
			goffset moved;
			moved = li_chunkqueue_steal_len(shr->stream.out, shr->stream.source->out, shr->content_length);
			shr->content_length -= moved;
		}
		if (shr->content_length == 0) {
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

LI_API liStream* li_stream_http_response_handle(liStream *http_in, liVRequest *vr, gboolean accept_cgi, gboolean accept_nph, gboolean keepalive) {
	liStreamHttpResponse *shr = g_slice_new0(liStreamHttpResponse);
	shr->keepalive = keepalive;
	shr->response_headers_finished = FALSE;
	shr->vr = vr;
	li_stream_init(&shr->stream, &vr->wrk->loop, stream_http_response_cb);
	li_http_response_parser_init(&shr->parse_response_ctx, &vr->response, http_in->out,
		accept_cgi, accept_nph);
	li_stream_connect(http_in, &shr->stream);
	return &shr->stream;
}

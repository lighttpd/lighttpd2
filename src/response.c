
#include <lighttpd/base.h>
#include <lighttpd/plugin_core.h>

void li_response_init(liResponse *resp) {
	resp->headers = li_http_headers_new();
	resp->http_status = 0;
	resp->transfer_encoding = LI_HTTP_TRANSFER_ENCODING_IDENTITY;
}

void li_response_reset(liResponse *resp) {
	li_http_headers_reset(resp->headers);
	resp->http_status = 0;
	resp->transfer_encoding = LI_HTTP_TRANSFER_ENCODING_IDENTITY;
}

void li_response_clear(liResponse *resp) {
	li_http_headers_free(resp->headers);
	resp->http_status = 0;
	resp->transfer_encoding = LI_HTTP_TRANSFER_ENCODING_IDENTITY;
}

void li_response_send_headers(liConnection *con) {
	GString *head;
	liVRequest *vr = con->mainvr;

	if (vr->response.http_status < 100 || vr->response.http_status > 999) {
		VR_ERROR(vr, "wrong status: %i", vr->response.http_status);
		con->response_headers_sent = FALSE;
		li_connection_internal_error(con);
		return;
	}

	head = g_string_sized_new(8*1024-1);

	if (0 == con->out->length && con->mainvr->handle_response_body == NULL
		&& vr->response.http_status >= 400 && vr->response.http_status < 600) {

		/*li_chunkqueue_append_mem(con->out, CONST_STR_LEN("Custom error\r\n"));*/
		li_response_send_error_page(con);
	}

	if (con->mainvr->handle_response_body == NULL) {
		con->out->is_closed = TRUE;
	}

	if ((vr->response.http_status >= 100 && vr->response.http_status < 200) ||
	     vr->response.http_status == 204 ||
	     vr->response.http_status == 205 ||
	     vr->response.http_status == 304) {
		/* They never have a content-body/length */
		li_chunkqueue_reset(con->out);
		con->out->is_closed = TRUE;
	} else if (con->out->is_closed) {
		g_string_printf(con->wrk->tmp_str, "%"L_GOFFSET_FORMAT, con->out->length);
		li_http_header_overwrite(vr->response.headers, CONST_STR_LEN("Content-Length"), GSTR_LEN(con->wrk->tmp_str));
	} else if (con->keep_alive && vr->request.http_version == LI_HTTP_VERSION_1_1) {
		/* TODO: maybe someone set a content length header? */
		if (!(vr->response.transfer_encoding & LI_HTTP_TRANSFER_ENCODING_CHUNKED)) {
			vr->response.transfer_encoding |= LI_HTTP_TRANSFER_ENCODING_CHUNKED;
			li_http_header_append(vr->response.headers, CONST_STR_LEN("Transfer-Encoding"), CONST_STR_LEN("chunked"));
		}
	} else {
		/* Unknown content length, no chunked encoding */
		con->keep_alive = FALSE;
	}

	if (vr->request.http_method == LI_HTTP_METHOD_HEAD) {
		/* content-length is set, but no body */
		li_chunkqueue_reset(con->out);
		con->out->is_closed = TRUE;
	}

	/* Status line */
	if (vr->request.http_version == LI_HTTP_VERSION_1_1) {
		g_string_append_len(head, CONST_STR_LEN("HTTP/1.1 "));
		if (!con->keep_alive)
			li_http_header_overwrite(vr->response.headers, CONST_STR_LEN("Connection"), CONST_STR_LEN("close"));
	} else {
		g_string_append_len(head, CONST_STR_LEN("HTTP/1.0 "));
		if (con->keep_alive)
			li_http_header_overwrite(vr->response.headers, CONST_STR_LEN("Connection"), CONST_STR_LEN("keep-alive"));
	}

	{
		guint len;
		gchar status_str[4];
		gchar *str = li_http_status_string(vr->response.http_status, &len);
		li_http_status_to_str(vr->response.http_status, status_str);
		status_str[3] = ' ';
		g_string_append_len(head, status_str, 4);
		g_string_append_len(head, str, len);
		g_string_append_len(head, CONST_STR_LEN("\r\n"));
	}

	/* Append headers */
	{
		liHttpHeader *header;
		GList *iter;
		gboolean have_date = FALSE, have_server = FALSE;

		for (iter = g_queue_peek_head_link(&vr->response.headers->entries); iter; iter = g_list_next(iter)) {
			header = (liHttpHeader*) iter->data;
			g_string_append_len(head, GSTR_LEN(header->data));
			g_string_append_len(head, CONST_STR_LEN("\r\n"));
			if (!have_date && http_header_key_is(header, CONST_STR_LEN("date"))) have_date = TRUE;
			if (!have_server && http_header_key_is(header, CONST_STR_LEN("server"))) have_server = TRUE;
		}

		if (!have_date) {
			GString *d = li_worker_current_timestamp(con->wrk, 0);
			/* HTTP/1.1 requires a Date: header */
			g_string_append_len(head, CONST_STR_LEN("Date: "));
			g_string_append_len(head, GSTR_LEN(d));
			g_string_append_len(head, CONST_STR_LEN("\r\n"));
		}

		if (!have_server) {
			GString *tag = CORE_OPTION(LI_CORE_OPTION_SERVER_TAG).string;

			if (tag->len) {
				g_string_append_len(head, CONST_STR_LEN("Server: "));
				g_string_append_len(head, GSTR_LEN(tag));
				g_string_append_len(head, CONST_STR_LEN("\r\n"));
			}
		}
	}

	g_string_append_len(head, CONST_STR_LEN("\r\n"));
	li_chunkqueue_append_string(con->raw_out, head);
}


void li_response_send_error_page(liConnection *con) {
	gchar status_str[3];
	guint len;
	gchar *str;

	li_chunkqueue_append_mem(con->out, CONST_STR_LEN(
		"<?xml version=\"1.0\" encoding=\"iso-8859-1\"?>\n"
		"<!DOCTYPE html PUBLIC \"-//W3C//DTD XHTML 1.0 Transitional//EN\"\n"
		"         \"http://www.w3.org/TR/xhtml1/DTD/xhtml1-transitional.dtd\">\n"
		"<html xmlns=\"http://www.w3.org/1999/xhtml\" xml:lang=\"en\" lang=\"en\">\n"
		" <head>\n"
		"  <title>"
	));

	li_http_status_to_str(con->mainvr->response.http_status, status_str);

	li_chunkqueue_append_mem(con->out, status_str, 3);
	li_chunkqueue_append_mem(con->out, CONST_STR_LEN(" - "));
	str = li_http_status_string(con->mainvr->response.http_status, &len);
	li_chunkqueue_append_mem(con->out, str, len);

	li_chunkqueue_append_mem(con->out, CONST_STR_LEN(
		"</title>\n"
		" </head>\n"
		" <body>\n"
		"  <h1>"
	));

	li_chunkqueue_append_mem(con->out, status_str, 3);
	li_chunkqueue_append_mem(con->out, CONST_STR_LEN(" - "));
	li_chunkqueue_append_mem(con->out, str, len);

	li_chunkqueue_append_mem(con->out, CONST_STR_LEN(
		"</h1>\n"
		" </body>\n"
		"</html>\n"
	));
}

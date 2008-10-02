
#include "base.h"
#include "utils.h"
#include "plugin_core.h"

void response_init(response *resp) {
	resp->headers = http_headers_new();
	resp->http_status = 0;
	resp->transfer_encoding = HTTP_TRANSFER_ENCODING_IDENTITY;
}

void response_reset(response *resp) {
	http_headers_reset(resp->headers);
	resp->http_status = 0;
	resp->transfer_encoding = HTTP_TRANSFER_ENCODING_IDENTITY;
}

void response_clear(response *resp) {
	http_headers_free(resp->headers);
	resp->http_status = 0;
	resp->transfer_encoding = HTTP_TRANSFER_ENCODING_IDENTITY;
}

void response_send_headers(connection *con) {
	GString *head = g_string_sized_new(8*1024);

	if (con->response.http_status < 100 || con->response.http_status > 999) {
		con->response.http_status = 500;
		con->content_handler = NULL;
		chunkqueue_reset(con->out);
	}

	if (0 == con->out->length && con->content_handler == NULL
		&& con->response.http_status >= 400 && con->response.http_status < 600) {

		/*chunkqueue_append_mem(con->out, CONST_STR_LEN("Custom error\r\n"));*/
		response_send_error_page(con);
	}

	if (con->content_handler == NULL) {
		con->out->is_closed = TRUE;
	}

	if ((con->response.http_status >= 100 && con->response.http_status < 200) ||
	     con->response.http_status == 204 ||
	     con->response.http_status == 205 ||
	     con->response.http_status == 304) {
		/* They never have a content-body/length */
		chunkqueue_reset(con->out);
		con->out->is_closed = TRUE;
	} else if (con->out->is_closed) {
		g_string_printf(con->wrk->tmp_str, "%"L_GOFFSET_FORMAT, con->out->length);
		http_header_overwrite(con->response.headers, CONST_STR_LEN("Content-Length"), GSTR_LEN(con->wrk->tmp_str));
	} else if (con->keep_alive && con->request.http_version == HTTP_VERSION_1_1) {
		if (!(con->response.transfer_encoding & HTTP_TRANSFER_ENCODING_CHUNKED)) {
			con->response.transfer_encoding |= HTTP_TRANSFER_ENCODING_CHUNKED;
			http_header_append(con->response.headers, CONST_STR_LEN("Transfer-Encoding"), CONST_STR_LEN("chunked"));
		}
	} else {
		/* Unknown content length, no chunked encoding */
		con->keep_alive = FALSE;
	}

	if (con->request.http_method == HTTP_METHOD_HEAD) {
		/* content-length is set, but no body */
		chunkqueue_reset(con->out);
		con->out->is_closed = TRUE;
	}

	/* Status line */
	if (con->request.http_version == HTTP_VERSION_1_1) {
		g_string_append_len(head, CONST_STR_LEN("HTTP/1.1 "));
		if (!con->keep_alive)
			http_header_overwrite(con->response.headers, CONST_STR_LEN("Connection"), CONST_STR_LEN("close"));
	} else {
		g_string_append_len(head, CONST_STR_LEN("HTTP/1.0 "));
		if (con->keep_alive)
			http_header_overwrite(con->response.headers, CONST_STR_LEN("Connection"), CONST_STR_LEN("keep-alive"));
	}

	{
		guint len;
		gchar status_str[4];
		gchar *str = http_status_string(con->response.http_status, &len);
		http_status_to_str(con->response.http_status, status_str);
		status_str[3] = ' ';
		g_string_append_len(head, status_str, 4);
		g_string_append_len(head, str, len);
		g_string_append_len(head, CONST_STR_LEN("\r\n"));
	}

	/* Append headers */
	{
		http_header *header;
		GList *iter;
		gboolean have_date = FALSE, have_server = FALSE;

		for (iter = g_queue_peek_head_link(&con->response.headers->entries); iter; iter = g_list_next(iter)) {
			header = (http_header*) iter->data;
			g_string_append_len(head, GSTR_LEN(header->data));
			g_string_append_len(head, CONST_STR_LEN("\r\n"));
			if (!have_date && http_header_key_is(header, CONST_STR_LEN("date"))) have_date = TRUE;
			if (!have_server && http_header_key_is(header, CONST_STR_LEN("server"))) have_server = TRUE;
		}

		if (!have_date) {
			GString *d = worker_current_timestamp(con->wrk);
			/* HTTP/1.1 requires a Date: header */
			g_string_append_len(head, CONST_STR_LEN("Date: "));
			g_string_append_len(head, GSTR_LEN(d));
			g_string_append_len(head, CONST_STR_LEN("\r\n"));
		}

		if (!have_server) {
			GString *tag = CORE_OPTION(CORE_OPTION_SERVER_TAG).string;

			if (tag->len) {
				g_string_append_len(head, CONST_STR_LEN("Server: "));
				g_string_append_len(head, GSTR_LEN(tag));
				g_string_append_len(head, CONST_STR_LEN("\r\n"));
			}
		}
	}

	g_string_append_len(head, CONST_STR_LEN("\r\n"));
	chunkqueue_append_string(con->raw_out, head);
}


void response_send_error_page(connection *con) {
	gchar status_str[3];
	guint len;
	gchar *str;

	chunkqueue_append_mem(con->out, CONST_STR_LEN(
		"<?xml version=\"1.0\" encoding=\"iso-8859-1\"?>\n"
		"<!DOCTYPE html PUBLIC \"-//W3C//DTD XHTML 1.0 Transitional//EN\"\n"
		"         \"http://www.w3.org/TR/xhtml1/DTD/xhtml1-transitional.dtd\">\n"
		"<html xmlns=\"http://www.w3.org/1999/xhtml\" xml:lang=\"en\" lang=\"en\">\n"
		" <head>\n"
		"  <title>"
	));

	http_status_to_str(con->response.http_status, status_str);

	chunkqueue_append_mem(con->out, status_str, 3);
	chunkqueue_append_mem(con->out, CONST_STR_LEN(" - "));
	str = http_status_string(con->response.http_status, &len);
	chunkqueue_append_mem(con->out, str, len);

	chunkqueue_append_mem(con->out, CONST_STR_LEN(
		"</title>\n"
		" </head>\n"
		" <body>\n"
		"  <h1>"
	));

	chunkqueue_append_mem(con->out, status_str, 3);
	chunkqueue_append_mem(con->out, CONST_STR_LEN(" - "));
	chunkqueue_append_mem(con->out, str, len);

	chunkqueue_append_mem(con->out, CONST_STR_LEN(
		"</h1>\n"
		" </body>\n"
		"</html>\n"
	));
}

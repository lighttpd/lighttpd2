
#include "base.h"
#include "utils.h"

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

void response_send_headers(server *srv, connection *con) {
	GString *head = g_string_sized_new(8*1024);

	if (con->response.http_status < 100 || con->response.http_status > 999) {
		con->response.http_status = 500;
		con->content_handler = NULL;
		chunkqueue_reset(con->out);
	}

	if (0 == con->out->length && con->content_handler == NULL
		&& con->response.http_status >= 400 && con->response.http_status < 600) {

		chunkqueue_append_mem(con->out, CONST_STR_LEN("Custom error\r\n"));
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
		g_string_printf(srv->tmp_str, "%"L_GOFFSET_FORMAT, con->out->length);
		http_header_overwrite(con->response.headers, CONST_STR_LEN("Content-Length"), GSTR_LEN(srv->tmp_str));
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
	g_string_append_printf(head, "%i %s\r\n", con->response.http_status, http_status_string(con->response.http_status));

	/* Append headers */
	{
		GHashTableIter iter;
		GString *key;
		http_header *header;
		GList *valiter;
		gboolean have_date = FALSE, have_server = FALSE;

		g_hash_table_iter_init (&iter, con->response.headers->table);
		while (g_hash_table_iter_next (&iter, (gpointer*) &key, (gpointer*) &header)) {
			if (!header) continue;

			valiter = g_queue_peek_head_link(&header->values);
			if (!valiter) continue;
			do {
				g_string_append_len(head, GSTR_LEN(header->key));
				g_string_append_len(head, CONST_STR_LEN(": "));
				g_string_append_len(head, GSTR_LEN((GString*) valiter->data));
				g_string_append_len(head, CONST_STR_LEN("\r\n"));
			} while (NULL != (valiter = g_list_next(valiter)));

			/* key is lowercase */
			if (0 == strcmp(key->str, "date")) have_date = TRUE;
			else if (0 == strcmp(key->str, "server")) have_server = TRUE;
		}

		if (!have_date) {
			GString *d = server_current_timestamp(srv);
			/* HTTP/1.1 requires a Date: header */
			g_string_append_len(head, CONST_STR_LEN("Date: "));
			g_string_append_len(head, GSTR_LEN(d));
			g_string_append_len(head, CONST_STR_LEN("\r\n"));
		}

		if (!have_server) {
			/* TODO: use option for this */
			g_string_append_len(head, CONST_STR_LEN("Server: "));
			g_string_append_len(head, CONST_STR_LEN("lighttpd-2.0~sandbox"));
			g_string_append_len(head, CONST_STR_LEN("\r\n"));
		}
	}

	g_string_append_len(head, CONST_STR_LEN("\r\n"));
	chunkqueue_append_string(con->raw_out, head);
}

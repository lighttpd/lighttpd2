
#include "base.h"

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
	GString *head = g_string_sized_new(4*1024);

	if (con->request.http_version == HTTP_VERSION_1_1) {
		g_string_append_len(head, CONST_STR_LEN("HTTP/1.1 "));
	} else {
		g_string_append_len(head, CONST_STR_LEN("HTTP/1.0 "));
	}

	if (con->response.http_status < 100 || con->response.http_status > 999) {
		con->response.http_status = 500;
		con->content_handler = NULL;
		chunkqueue_reset(con->out);
	}

	if (0 == con->out->length && con->content_handler == NULL
		&& con->response.http_status >= 400 && con->response.http_status < 600) {
		
		chunkqueue_append_mem(con->out, CONST_STR_LEN("Custom error"));
	}

	if (con->content_handler == NULL) {
		con->out->is_closed = TRUE;
	}

	g_string_append_printf(head, "%i XXX\r\n", con->response.http_status);

	/* TODO: append headers */

	g_string_append_len(head, CONST_STR_LEN("\r\n"));
	chunkqueue_append_string(con->raw_out, head);
}

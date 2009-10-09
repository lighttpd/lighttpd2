
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

gboolean li_response_send_headers(liConnection *con) {
	GString *head;
	liVRequest *vr = con->mainvr;

	if (vr->response.http_status < 100 || vr->response.http_status > 999) {
		VR_ERROR(vr, "wrong status: %i", vr->response.http_status);
		return FALSE;
	}

	head = g_string_sized_new(8*1024-1);

	if (0 == con->out->length && con->mainvr->backend == NULL
		&& vr->response.http_status >= 400 && vr->response.http_status < 600) {
		li_response_send_error_page(con);
	}

	if ((vr->response.http_status >= 100 && vr->response.http_status < 200) ||
	     vr->response.http_status == 204 ||
	     vr->response.http_status == 205 ||
	     vr->response.http_status == 304) {
		/* They never have a content-body/length */
		li_chunkqueue_reset(con->out);
		con->out->is_closed = TRUE;
	} else if (con->out->is_closed && (vr->request.http_method != LI_HTTP_METHOD_HEAD || con->out->length > 0)) {
		/* do not send content-length: 0 if backend already skipped content generation for HEAD */
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
			if (!have_date && li_http_header_key_is(header, CONST_STR_LEN("date"))) have_date = TRUE;
			if (!have_server && li_http_header_key_is(header, CONST_STR_LEN("server"))) have_server = TRUE;
		}

		if (!have_date) {
			GString *d = li_worker_current_timestamp(con->wrk, LI_GMTIME, LI_TS_FORMAT_HEADER);
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

	return TRUE;
}

#define SET_LEN_AND_RETURN_STR(x) \
	do { \
		*len = sizeof(x) - 1; \
		return x; \
	} while(0)

static gchar *li_response_error_description(guint status_code, guint *len) {
	switch (status_code) {
	/* 4XX client error */
	case 400: SET_LEN_AND_RETURN_STR("<p>Your browser sent a request that this server could not understand.</p>");
	case 401: SET_LEN_AND_RETURN_STR(
		"<p>This server could not verify that you are authorized to access the resource requested.<br>"
		"Either you supplied the wrong credentials (e.g. bad password), or your browser doesn't understand how to supply the credentials required.</p>"
	);
	case 402: SET_LEN_AND_RETURN_STR("<p>Insert coint.</p>");
	case 403: SET_LEN_AND_RETURN_STR("<p>You don't have permission to access the requested resource.</p>");
	case 404: SET_LEN_AND_RETURN_STR("<p>The requested resource was not found.<br>Make sure you entered the correct URL in your browser.</p>\n");
	case 405: SET_LEN_AND_RETURN_STR("<p>The requested method is not allowed for this resource.</p>");
	case 406: SET_LEN_AND_RETURN_STR("<p>An appropriate representation of the requested resource could not be found.</p>");
	case 407: SET_LEN_AND_RETURN_STR(
		"<p>This server could not verify that you are authorized to access the resource requested.<br>"
		"Either you supplied the wrong credentials (e.g. bad password), or your browser doesn't understand how to supply the credentials required.</p>"
	);
	case 408: SET_LEN_AND_RETURN_STR("<p>Timeout waiting for the request.</p>");
	case 409: SET_LEN_AND_RETURN_STR("<p>The request could not be completed due to a conflict with the current state of the requested resource.</p>");
	case 410: SET_LEN_AND_RETURN_STR(
		"<p>The requested resource is no longer available on this server and there is no forwarding address.<br>"
		"Please remove all references to this resource.</p>"
	);
	case 411: SET_LEN_AND_RETURN_STR("<p>The requested method requires a valid Content-Length header.</p>");
	case 412: SET_LEN_AND_RETURN_STR("<p>The precondition for this request was not met.</p>");
	case 413: SET_LEN_AND_RETURN_STR("<p>Your client sent too much data for this request.</p>");
	case 414: SET_LEN_AND_RETURN_STR("<p>The requested URL's length exceeds the configured limit of this server.</p>");
	case 415: SET_LEN_AND_RETURN_STR("<p>The supplied request data is not in a format acceptable for processing by this resource.</p>");
	case 416: SET_LEN_AND_RETURN_STR("<p>The requested part of this resource was not found.</p>");
	case 417: SET_LEN_AND_RETURN_STR("<p>The expectation given in the Expect request-header field could not be met by this server.</p>");
	case 418: SET_LEN_AND_RETURN_STR("<p>I have come here to brew coffee and kick ass... and I'm all out of coffee.</p>");
	case 421: SET_LEN_AND_RETURN_STR("<p>There are too many connections from your internet address.</p>");
	case 422: SET_LEN_AND_RETURN_STR("<p>The request could not be processed.</p>");
	case 423: SET_LEN_AND_RETURN_STR("<p>The requested resource is currently locked.</p>");
	case 424: SET_LEN_AND_RETURN_STR("<p>The method could not be performed on the resource because the requested action depended on another action and that other action failed.</p>");
	case 426: SET_LEN_AND_RETURN_STR("<p>This resource should be accessed via SSL.</p>");
	/* 5XX server error */
	case 500: SET_LEN_AND_RETURN_STR("<p>The server encountered an internal error and was unable to complete your request.</p");
	case 501: SET_LEN_AND_RETURN_STR("<p>The request method is not supported by this server.</p>");
	case 502: SET_LEN_AND_RETURN_STR("<p>The proxy server received an invalid response from an upstream server.</p>");
	case 503: SET_LEN_AND_RETURN_STR("<p>The server is temporarily unable to service your request due to maintenance downtime or capacity problems.<br>Please try again later.</p>");
	case 504: SET_LEN_AND_RETURN_STR("<p>The proxy server did not receive a timely response from the upstream server.</p>");
	case 505: SET_LEN_AND_RETURN_STR("<p>The requested http version is not supported by this server.</p>");
	case 506: SET_LEN_AND_RETURN_STR("<p>A variant for the requested resource is itself a negotiable resource.</p");
	case 507: SET_LEN_AND_RETURN_STR("<p>The request could not be fullfilled because of insufficient storage available on the server.</p>");
	case 509: SET_LEN_AND_RETURN_STR("<p>The available bandwidth limit for this resource has been reached.</p>");
	case 510: SET_LEN_AND_RETURN_STR("<p>A mandatory extension policy in the request is not accepted by the server for this resource.</p>");

	/* unknown */
	default: SET_LEN_AND_RETURN_STR("<p></p>");
	}
}

void li_response_send_error_page(liConnection *con) {
	liVRequest *vr = con->mainvr;
	gchar status_code[3];
	guint len;
	gchar *str;
	GString *html;

	html = g_string_sized_new(1023);

	g_string_append_len(html, CONST_STR_LEN(
		"<!DOCTYPE HTML PUBLIC \"-//W3C//DTD HTML 4.01//EN\" \"http://www.w3.org/TR/html4/strict.dtd\">\n"
		"<html>\n"
		"	<head>\n"
		"		<title>"
	));

	li_http_status_to_str(con->mainvr->response.http_status, status_code);

	g_string_append_len(html, status_code, 3);
	g_string_append_len(html, CONST_STR_LEN(" - "));
	str = li_http_status_string(con->mainvr->response.http_status, &len);
	g_string_append_len(html, str, len);

	g_string_append_len(html, CONST_STR_LEN(
		"</title>\n"
		"		<style type=\"text/css\">\n"
		"			body { font-size: 62.5%; }\n"
		"			#container {\n"
		"				font-size: 62.5%;\n"
		"				max-width: 600px;\n"
		"				margin: auto;\n"
		"				margin-top: 2%;\n"
		"				border: 4px solid #efefef;\n"
		"				padding: 0px 20px;\n"
		"				color: #444;\n"
		"				font-family: Verdana,helvetica,sans-serif;\n"
		"				font-size: 1.25em;\n"
		"			}\n"
		"			h1 { color: #6D84B4; font-size: 1.5em; }\n"
		"			#footer { text-align: right; margin-top: 25px; }\n"
		"		</style>\n"
		"	</head>\n"
		"	<body>\n"
		"		<div id=\"container\">\n"
		"			<h1>Error "
	));

	g_string_append_len(html, status_code, 3);
	g_string_append_len(html, CONST_STR_LEN(" - "));
	g_string_append_len(html, str, len);
	g_string_append_len(html, CONST_STR_LEN("</h1>\n"));

	str = li_response_error_description(con->mainvr->response.http_status, &len);
	g_string_append_len(html, str, len);
	
	g_string_append_len(html, CONST_STR_LEN("			<p id=\"footer\">"));
	g_string_append_len(html, GSTR_LEN(CORE_OPTION(LI_CORE_OPTION_SERVER_TAG).string));
	g_string_append_len(html, CONST_STR_LEN(
		"</p>\n"
		"		</div>\n"
		"	</body>\n"
		"</html>\n"
	));

	li_chunkqueue_append_string(con->out, html);
}


#include "base.h"
#include "url_parser.h"
#include "utils.h"

void request_init(request *req, chunkqueue *in) {
	req->http_method = HTTP_METHOD_UNSET;
	req->http_method_str = g_string_sized_new(0);
	req->http_version = HTTP_VERSION_UNSET;

	req->uri.raw = g_string_sized_new(0);
	req->uri.scheme = g_string_sized_new(0);
	req->uri.authority = g_string_sized_new(0);
	req->uri.path = g_string_sized_new(0);
	req->uri.query = g_string_sized_new(0);
	req->uri.host = g_string_sized_new(0);

	req->headers = http_headers_new();

	req->content_length = -1;

	http_request_parser_init(&req->parser_ctx, req, in);
}

void request_reset(request *req) {
	req->http_method = HTTP_METHOD_UNSET;
	g_string_truncate(req->http_method_str, 0);
	req->http_version = HTTP_VERSION_UNSET;

	g_string_truncate(req->uri.raw, 0);
	g_string_truncate(req->uri.scheme, 0);
	g_string_truncate(req->uri.authority, 0);
	g_string_truncate(req->uri.path, 0);
	g_string_truncate(req->uri.query, 0);
	g_string_truncate(req->uri.host, 0);

	http_headers_reset(req->headers);

	req->content_length = -1;

	http_request_parser_reset(&req->parser_ctx);
}

void request_clear(request *req) {
	req->http_method = HTTP_METHOD_UNSET;
	g_string_free(req->http_method_str, TRUE);
	req->http_version = HTTP_VERSION_UNSET;

	g_string_free(req->uri.raw, TRUE);
	g_string_free(req->uri.scheme, TRUE);
	g_string_free(req->uri.authority, TRUE);
	g_string_free(req->uri.path, TRUE);
	g_string_free(req->uri.query, TRUE);
	g_string_free(req->uri.host, TRUE);

	http_headers_free(req->headers);

	req->content_length = -1;

	http_request_parser_clear(&req->parser_ctx);
}

/* closes connection after response */
static void bad_request(server *srv, connection *con, int status) {
	con->keep_alive = FALSE;
	con->response.http_status = status;
	connection_handle_direct(srv, con);
}

gboolean request_parse_url(server *srv, connection *con) {
	request *req = &con->request;
	UNUSED(srv); UNUSED(req);

	g_string_truncate(req->uri.query, 0);
	g_string_truncate(req->uri.path, 0);

	if (!parse_raw_url(&req->uri))
		return FALSE;

	/* "*" only allowed for method OPTIONS */
	if (0 == strcmp(req->uri.path->str, "*") && req->http_method != HTTP_METHOD_OPTIONS)
		return FALSE;

	url_decode(req->uri.path);
	path_simplify(req->uri.path);

	return TRUE;
}

void request_validate_header(server *srv, connection *con) {
	request *req = &con->request;
	http_header *hh;

	switch (req->http_version) {
	case HTTP_VERSION_1_0:
		if (!http_header_is(req->headers, CONST_STR_LEN("connection"), CONST_STR_LEN("keep-alive")))
			con->keep_alive = FALSE;
		break;
	case HTTP_VERSION_1_1:
		if (http_header_is(req->headers, CONST_STR_LEN("connection"), CONST_STR_LEN("close")))
			con->keep_alive = FALSE;
		break;
	case HTTP_VERSION_UNSET:
		bad_request(srv, con, 505); /* Version not Supported */
		return;
	}

	if (req->uri.raw->len == 0) {
		bad_request(srv, con, 400); /* bad request */
		return;
	}

	/* get hostname */
	hh = http_header_lookup_fast(req->headers, CONST_STR_LEN("host"));
	if (hh && hh->values.length != 1) {
		bad_request(srv, con, 400); /* bad request */
		return;
	} else if (hh) {
		g_string_append_len(req->uri.host, GSTR_LEN((GString*) g_queue_peek_head(&hh->values)));
		if (parse_hostname(&req->uri)) {
			bad_request(srv, con, 400); /* bad request */
			return;
		}
	}

	/* Need hostname in HTTP/1.1 */
	if (req->uri.host->len == 0 && req->http_version == HTTP_VERSION_1_1) {
		bad_request(srv, con, 400); /* bad request */
		return;
	}

	/* may override hostname */
	if (!request_parse_url(srv, con)) {
		bad_request(srv, con, 400); /* bad request */
		return;
	}

	/* content-length */
	hh = http_header_lookup_fast(req->headers, CONST_STR_LEN("content-length"));
	if (hh) {
		GString *val = (GString*) g_queue_peek_head(&hh->values);
		off_t r;
		char *err;

		r = str_to_off_t(val->str, &err, 10);
		if (*err != '\0') {
			CON_TRACE(srv, con, "content-length is not a number: %s (Status: 400)", err);
			bad_request(srv, con, 400); /* bad request */
			return;
		}

		/**
			* negative content-length is not supported 
			* and is a bad request
			*/
		if (r < 0) {
			bad_request(srv, con, 400); /* bad request */
			return;
		}

		/**
			* check if we had a over- or underrun in the string conversion
			*/
		if (r == STR_OFF_T_MIN ||
			r == STR_OFF_T_MAX) {
			if (errno == ERANGE) {
				bad_request(srv, con, 413); /* Request Entity Too Large */
				return;
			}
		}

		con->request.content_length = r;
	}

	/* Expect: 100-continue */
	hh = http_header_lookup_fast(req->headers, CONST_STR_LEN("expect"));
	if (hh) {
		GList *iter;
		gboolean expect_100_cont = FALSE;

		for (iter = g_queue_peek_head_link(&hh->values); NULL != iter; iter = g_list_next(iter)) {
			if (0 == strcasecmp( ((GString*)iter->data)->str, "100-continue" )) {
				expect_100_cont = TRUE;
			} else {
				/* we only support 100-continue */
				bad_request(srv, con, 417); /* Expectation Failed */
				return;
			}
		}

		if (expect_100_cont && req->http_version == HTTP_VERSION_1_0) {
			/* only HTTP/1.1 clients can send us this header */
			bad_request(srv, con, 417); /* Expectation Failed */
			return;
		}
		con->expect_100_cont = expect_100_cont;
	}

	/* TODO: headers:
	 * - If-Modified-Since (different duplicate check)
	 * - If-None-Match (different duplicate check)
	 * - Range (duplicate check)
	 */

	switch(con->request.http_method) {
	case HTTP_METHOD_GET:
	case HTTP_METHOD_HEAD:
		/* content-length is forbidden for those */
		if (con->request.content_length > 0) {
			CON_ERROR(srv, con, "%s", "GET/HEAD with content-length -> 400");

			bad_request(srv, con, 400); /* bad request */
			return;
		}
		con->request.content_length = 0;
		break;
	case HTTP_METHOD_POST:
		/* content-length is required for them */
		if (con->request.content_length == -1) {
			/* content-length is missing */
			CON_ERROR(srv, con, "%s", "POST-request, but content-length missing -> 411");

			bad_request(srv, con, 411); /* Length Required */
			return;
		}
		break;
	default:
		/* the may have a content-length */
		break;
	}
}

void physical_init(physical *phys) {
	phys->path = g_string_sized_new(512);
	phys->basedir = g_string_sized_new(256);
	phys->doc_root = g_string_sized_new(256);
	phys->rel_path = g_string_sized_new(256);
	phys->pathinfo = g_string_sized_new(256);
	phys->size = -1;
}

void physical_reset(physical *phys) {
	g_string_truncate(phys->path, 0);
	g_string_truncate(phys->basedir, 0);
	g_string_truncate(phys->doc_root, 0);
	g_string_truncate(phys->rel_path, 0);
	g_string_truncate(phys->pathinfo, 0);
	phys->size = -1;
}

void physical_clear(physical *phys) {
	g_string_free(phys->path, TRUE);
	g_string_free(phys->basedir, TRUE);
	g_string_free(phys->doc_root, TRUE);
	g_string_free(phys->rel_path, TRUE);
	g_string_free(phys->pathinfo, TRUE);
	phys->size = -1;
}

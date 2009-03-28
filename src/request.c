
#include <lighttpd/base.h>
#include <lighttpd/url_parser.h>

void request_init(request *req) {
	req->http_method = HTTP_METHOD_UNSET;
	req->http_method_str = g_string_sized_new(0);
	req->http_version = HTTP_VERSION_UNSET;

	req->uri.raw = g_string_sized_new(0);
	req->uri.scheme = g_string_sized_new(0);
	req->uri.authority = g_string_sized_new(0);
	req->uri.path = g_string_sized_new(0);
	req->uri.orig_path = g_string_sized_new(0);
	req->uri.query = g_string_sized_new(0);
	req->uri.host = g_string_sized_new(0);

	req->headers = http_headers_new();

	req->content_length = -1;
}

void request_reset(request *req) {
	req->http_method = HTTP_METHOD_UNSET;
	g_string_truncate(req->http_method_str, 0);
	req->http_version = HTTP_VERSION_UNSET;

	g_string_truncate(req->uri.raw, 0);
	g_string_truncate(req->uri.scheme, 0);
	g_string_truncate(req->uri.authority, 0);
	g_string_truncate(req->uri.path, 0);
	g_string_truncate(req->uri.orig_path, 0);
	g_string_truncate(req->uri.query, 0);
	g_string_truncate(req->uri.host, 0);

	http_headers_reset(req->headers);

	req->content_length = -1;
}

void request_clear(request *req) {
	req->http_method = HTTP_METHOD_UNSET;
	g_string_free(req->http_method_str, TRUE);
	req->http_version = HTTP_VERSION_UNSET;

	g_string_free(req->uri.raw, TRUE);
	g_string_free(req->uri.scheme, TRUE);
	g_string_free(req->uri.authority, TRUE);
	g_string_free(req->uri.path, TRUE);
	g_string_free(req->uri.orig_path, TRUE);
	g_string_free(req->uri.query, TRUE);
	g_string_free(req->uri.host, TRUE);

	http_headers_free(req->headers);

	req->content_length = -1;
}

/* closes connection after response */
static void bad_request(connection *con, int status) {
	con->keep_alive = FALSE;
	con->mainvr->response.http_status = status;
	vrequest_handle_direct(con->mainvr);
}

static gboolean request_parse_url(vrequest *vr) {
	request *req = &vr->request;

	g_string_truncate(req->uri.query, 0);
	g_string_truncate(req->uri.path, 0);

	if (!parse_raw_url(&req->uri))
		return FALSE;

	/* "*" only allowed for method OPTIONS */
	if (0 == strcmp(req->uri.path->str, "*") && req->http_method != HTTP_METHOD_OPTIONS)
		return FALSE;

	url_decode(req->uri.path);
	path_simplify(req->uri.path);

	if (0 == req->uri.orig_path->len) {
		g_string_append_len(req->uri.orig_path, GSTR_LEN(req->uri.path)); /* save orig path */
	}

	return TRUE;
}

gboolean request_validate_header(connection *con) {
	request *req = &con->mainvr->request;
	http_header *hh;
	GList *l;

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
		bad_request(con, 505); /* Version not Supported */
		return FALSE;
	}

	if (req->uri.raw->len == 0) {
		bad_request(con, 400); /* bad request */
		return FALSE;
	}

	/* get hostname */
	l = http_header_find_first(req->headers, CONST_STR_LEN("host"));
	if (NULL != l) {
		if (NULL != http_header_find_next(l, CONST_STR_LEN("host"))) {
			/* more than one "host" header */
			bad_request(con, 400); /* bad request */
			return FALSE;
		}

		hh = (http_header*) l->data;
		g_string_append_len(req->uri.authority, HEADER_VALUE_LEN(hh));
		if (!parse_hostname(&req->uri)) {
			bad_request(con, 400); /* bad request */
			return FALSE;
		}
	}

	/* Need hostname in HTTP/1.1 */
	if (req->uri.host->len == 0 && req->http_version == HTTP_VERSION_1_1) {
		bad_request(con, 400); /* bad request */
		return FALSE;
	}

	/* may override hostname */
	if (!request_parse_url(con->mainvr)) {
		bad_request(con, 400); /* bad request */
		return FALSE;
	}

	/* content-length */
	hh = http_header_lookup(req->headers, CONST_STR_LEN("content-length"));
	if (hh) {
		const gchar *val = HEADER_VALUE(hh);
		off_t r;
		char *err;

		r = str_to_off_t(val, &err, 10);
		if (*err != '\0') {
			_DEBUG(con->srv, con->mainvr, "content-length is not a number: %s (Status: 400)", err);
			bad_request(con, 400); /* bad request */
			return FALSE;
		}

		/**
			* negative content-length is not supported
			* and is a bad request
			*/
		if (r < 0) {
			bad_request(con, 400); /* bad request */
			return FALSE;
		}

		/**
			* check if we had a over- or underrun in the string conversion
			*/
		if (r == STR_OFF_T_MIN ||
			r == STR_OFF_T_MAX) {
			if (errno == ERANGE) {
				bad_request(con, 413); /* Request Entity Too Large */
				return FALSE;
			}
		}

		con->mainvr->request.content_length = r;
	}

	/* Expect: 100-continue */
	l = http_header_find_first(req->headers, CONST_STR_LEN("expect"));
	if (l) {
		gboolean expect_100_cont = FALSE;

		for ( ; l ; l = http_header_find_next(l, CONST_STR_LEN("expect")) ) {
			hh = (http_header*) l->data;
			if (0 == strcasecmp( HEADER_VALUE(hh), "100-continue" )) {
				expect_100_cont = TRUE;
			} else {
				/* we only support 100-continue */
				bad_request(con, 417); /* Expectation Failed */
				return FALSE;
			}
		}

		if (expect_100_cont && req->http_version == HTTP_VERSION_1_0) {
			/* only HTTP/1.1 clients can send us this header */
			bad_request(con, 417); /* Expectation Failed */
			return FALSE;
		}
		con->expect_100_cont = expect_100_cont;
	}

	/* TODO: headers:
	 * - If-Modified-Since (different duplicate check)
	 * - If-None-Match (different duplicate check)
	 * - Range (duplicate check)
	 */

	switch(con->mainvr->request.http_method) {
	case HTTP_METHOD_GET:
	case HTTP_METHOD_HEAD:
		/* content-length is forbidden for those */
		if (con->mainvr->request.content_length > 0) {
			VR_ERROR(con->mainvr, "%s", "GET/HEAD with content-length -> 400");

			bad_request(con, 400); /* bad request */
			return FALSE;
		}
		con->mainvr->request.content_length = 0;
		break;
	case HTTP_METHOD_POST:
		/* content-length is required for them */
		if (con->mainvr->request.content_length == -1) {
			/* content-length is missing */
			VR_ERROR(con->mainvr, "%s", "POST-request, but content-length missing -> 411");

			bad_request(con, 411); /* Length Required */
			return FALSE;
		}
		break;
	default:
		/* the may have a content-length */
		break;
	}

	return TRUE;
}

void physical_init(physical *phys) {
	phys->path = g_string_sized_new(127);
	phys->basedir = g_string_sized_new(63);
	phys->doc_root = g_string_sized_new(63);
	phys->rel_path = g_string_sized_new(63);
	phys->pathinfo = g_string_sized_new(63);
	phys->have_stat = FALSE;
	phys->have_errno = FALSE;
}

void physical_reset(physical *phys) {
	g_string_truncate(phys->path, 0);
	g_string_truncate(phys->basedir, 0);
	g_string_truncate(phys->doc_root, 0);
	g_string_truncate(phys->rel_path, 0);
	g_string_truncate(phys->pathinfo, 0);
	phys->have_stat = FALSE;
	phys->have_errno = FALSE;
}

void physical_clear(physical *phys) {
	g_string_free(phys->path, TRUE);
	g_string_free(phys->basedir, TRUE);
	g_string_free(phys->doc_root, TRUE);
	g_string_free(phys->rel_path, TRUE);
	g_string_free(phys->pathinfo, TRUE);
	phys->have_stat = FALSE;
	phys->have_errno = FALSE;
}

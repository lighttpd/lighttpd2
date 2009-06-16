
#include <lighttpd/base.h>
#include <lighttpd/plugin_core.h>

#define SET_LEN_AND_RETURN_STR(x) \
	do { \
		*len = sizeof(x) - 1; \
		return x; \
	} while(0)

gchar *http_status_string(guint status_code, guint *len) {
	/* RFC 2616 (as well as RFC 2518, RFC 2817, RFC 2295, RFC 2774, RFC 4918) */
	switch (status_code) {
	/* 1XX information */
	case 100: SET_LEN_AND_RETURN_STR("Continue");
	case 101: SET_LEN_AND_RETURN_STR("Switching Protocols");
	case 102: SET_LEN_AND_RETURN_STR("Processing");
	/* 2XX successful operation */
	case 200: SET_LEN_AND_RETURN_STR("OK");
	case 201: SET_LEN_AND_RETURN_STR("Created");
	case 202: SET_LEN_AND_RETURN_STR("Accepted");
	case 203: SET_LEN_AND_RETURN_STR("Non-Authoritative Information");
	case 204: SET_LEN_AND_RETURN_STR("No Content");
	case 205: SET_LEN_AND_RETURN_STR("Reset Content");
	case 206: SET_LEN_AND_RETURN_STR("Partial Content");
	case 207: SET_LEN_AND_RETURN_STR("Multi-Status");
	/* 3XX redirect */
	case 300: SET_LEN_AND_RETURN_STR("Multiple Choice");
	case 301: SET_LEN_AND_RETURN_STR("Moved Permanently");
	case 302: SET_LEN_AND_RETURN_STR("Found");
	case 303: SET_LEN_AND_RETURN_STR("See Other");
	case 304: SET_LEN_AND_RETURN_STR("Not Modified");
	case 305: SET_LEN_AND_RETURN_STR("Use Proxy");
	case 306: SET_LEN_AND_RETURN_STR("(reserved)");
	case 307: SET_LEN_AND_RETURN_STR("Temporary Redirect");
	/* 4XX client error */
	case 400: SET_LEN_AND_RETURN_STR("Bad Request");
	case 401: SET_LEN_AND_RETURN_STR("Unauthorized");
	case 402: SET_LEN_AND_RETURN_STR("Payment Required");
	case 403: SET_LEN_AND_RETURN_STR("Forbidden");
	case 404: SET_LEN_AND_RETURN_STR("Not Found");
	case 405: SET_LEN_AND_RETURN_STR("Method Not Allowed");
	case 406: SET_LEN_AND_RETURN_STR("Not Acceptable");
	case 407: SET_LEN_AND_RETURN_STR("Proxy Authentication Required");
	case 408: SET_LEN_AND_RETURN_STR("Request Time-out");
	case 409: SET_LEN_AND_RETURN_STR("Conflict");
	case 410: SET_LEN_AND_RETURN_STR("Gone");
	case 411: SET_LEN_AND_RETURN_STR("Length Required");
	case 412: SET_LEN_AND_RETURN_STR("Precondition Failed");
	case 413: SET_LEN_AND_RETURN_STR("Request Entity Too Large");
	case 414: SET_LEN_AND_RETURN_STR("Request-URI Too Long");
	case 415: SET_LEN_AND_RETURN_STR("Unsupported Media Type");
	case 416: SET_LEN_AND_RETURN_STR("Request range not satisfiable");
	case 417: SET_LEN_AND_RETURN_STR("Expectation Failed");
	case 421: SET_LEN_AND_RETURN_STR("There are too many connections from your internet address");
	case 422: SET_LEN_AND_RETURN_STR("Unprocessable Entity");
	case 423: SET_LEN_AND_RETURN_STR("Locked");
	case 424: SET_LEN_AND_RETURN_STR("Failed Dependency");
	case 425: SET_LEN_AND_RETURN_STR("Unordered Collection");
	case 426: SET_LEN_AND_RETURN_STR("Upgrade Required");
	/* 5XX server error */
	case 500: SET_LEN_AND_RETURN_STR("Internal Server Error");
	case 501: SET_LEN_AND_RETURN_STR("Not Implemented");
	case 502: SET_LEN_AND_RETURN_STR("Bad Gateway");
	case 503: SET_LEN_AND_RETURN_STR("Service Unavailable");
	case 504: SET_LEN_AND_RETURN_STR("Gateway Time-out");
	case 505: SET_LEN_AND_RETURN_STR("HTTP Version not supported");
	case 506: SET_LEN_AND_RETURN_STR("Variant Also Negotiates");
	case 507: SET_LEN_AND_RETURN_STR("Insufficient Storage");
	case 509: SET_LEN_AND_RETURN_STR("Bandwidth Limit Exceeded");
	case 510: SET_LEN_AND_RETURN_STR("Not Extended");

	/* unknown */
	default: SET_LEN_AND_RETURN_STR("unknown status");
	}
}

gchar *http_method_string(http_method_t method, guint *len) {
	switch (method) {
	case HTTP_METHOD_UNSET:           SET_LEN_AND_RETURN_STR("UNKNOWN");
	case HTTP_METHOD_GET:             SET_LEN_AND_RETURN_STR("GET");
	case HTTP_METHOD_POST:            SET_LEN_AND_RETURN_STR("POST");
	case HTTP_METHOD_HEAD:            SET_LEN_AND_RETURN_STR("HEAD");
	case HTTP_METHOD_OPTIONS:         SET_LEN_AND_RETURN_STR("OPTIONS");
	case HTTP_METHOD_PROPFIND:        SET_LEN_AND_RETURN_STR("PROPFIND");
	case HTTP_METHOD_MKCOL:           SET_LEN_AND_RETURN_STR("MKCOL");
	case HTTP_METHOD_PUT:             SET_LEN_AND_RETURN_STR("PUT");
	case HTTP_METHOD_DELETE:          SET_LEN_AND_RETURN_STR("DELETE");
	case HTTP_METHOD_COPY:            SET_LEN_AND_RETURN_STR("COPY");
	case HTTP_METHOD_MOVE:            SET_LEN_AND_RETURN_STR("MOVE");
	case HTTP_METHOD_PROPPATCH:       SET_LEN_AND_RETURN_STR("PROPPATCH");
	case HTTP_METHOD_REPORT:          SET_LEN_AND_RETURN_STR("REPORT");
	case HTTP_METHOD_CHECKOUT:        SET_LEN_AND_RETURN_STR("CHECKOUT");
	case HTTP_METHOD_CHECKIN:         SET_LEN_AND_RETURN_STR("CHECKIN");
	case HTTP_METHOD_VERSION_CONTROL: SET_LEN_AND_RETURN_STR("VERSION_CONTROL");
	case HTTP_METHOD_UNCHECKOUT:      SET_LEN_AND_RETURN_STR("UNCHECKOUT");
	case HTTP_METHOD_MKACTIVITY:      SET_LEN_AND_RETURN_STR("MKACTIVITY");
	case HTTP_METHOD_MERGE:           SET_LEN_AND_RETURN_STR("MERGE");
	case HTTP_METHOD_LOCK:            SET_LEN_AND_RETURN_STR("LOCK");
	case HTTP_METHOD_UNLOCK:          SET_LEN_AND_RETURN_STR("UNLOCK");
	case HTTP_METHOD_LABEL:           SET_LEN_AND_RETURN_STR("LABEL");
	case HTTP_METHOD_CONNECT:         SET_LEN_AND_RETURN_STR("CONNECT");
	}

	*len = 0;
	return NULL;
}

gchar *http_version_string(http_version_t method, guint *len) {
	switch (method) {
	case HTTP_VERSION_1_1: SET_LEN_AND_RETURN_STR("HTTP/1.1");
	case HTTP_VERSION_1_0: SET_LEN_AND_RETURN_STR("HTTP/1.0");
	case HTTP_VERSION_UNSET: SET_LEN_AND_RETURN_STR("HTTP/??");
	}

	*len = 0;
	return NULL;
}

#undef SET_LEN_AND_RETURN_STR

void http_status_to_str(gint status_code, gchar status_str[]) {
	status_str[2] = status_code % 10 + '0';
	status_code /= 10;
	status_str[1] = status_code % 10 + '0';
	status_code /= 10;
	status_str[0] = status_code + '0';
}


GString *mimetype_get(vrequest *vr, GString *filename) {
	/* search in mime_types option for the first match */
	GArray *arr;

	if (!vr || !filename || !filename->len)
		return NULL;

	arr = CORE_OPTION(CORE_OPTION_MIME_TYPES).list;

	for (guint i = 0; i < arr->len; i++) {
		gint k, j;
		value *tuple = g_array_index(arr, value*, i);
		GString *ext = g_array_index(tuple->data.list, value*, 0)->data.string;
		if (ext->len > filename->len)
			continue;

		/* "" extension matches everything, used for default mimetype */
		if (!ext->len)
			return g_array_index(tuple->data.list, value*, 1)->data.string;

		k = filename->len - 1;
		j = ext->len - 1;
		for (; j >= 0; j--) {
			if (ext->str[j] != filename->str[k])
				break;
			k--;
		}

		if (j == -1)
			return g_array_index(tuple->data.list, value*, 1)->data.string;
	}

	return NULL;
}

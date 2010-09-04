
#include <lighttpd/base.h>
#include <lighttpd/plugin_core.h>

#define SET_LEN_AND_RETURN_STR(x) \
	do { \
		*len = sizeof(x) - 1; \
		return x; \
	} while(0)

gchar *li_http_status_string(guint status_code, guint *len) {
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
	case 418: SET_LEN_AND_RETURN_STR("I'm a teapot");
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

gchar *li_http_method_string(liHttpMethod method, guint *len) {
	switch (method) {
	case LI_HTTP_METHOD_UNSET:           SET_LEN_AND_RETURN_STR("UNKNOWN");
	case LI_HTTP_METHOD_GET:             SET_LEN_AND_RETURN_STR("GET");
	case LI_HTTP_METHOD_POST:            SET_LEN_AND_RETURN_STR("POST");
	case LI_HTTP_METHOD_HEAD:            SET_LEN_AND_RETURN_STR("HEAD");
	case LI_HTTP_METHOD_OPTIONS:         SET_LEN_AND_RETURN_STR("OPTIONS");
	case LI_HTTP_METHOD_PROPFIND:        SET_LEN_AND_RETURN_STR("PROPFIND");
	case LI_HTTP_METHOD_MKCOL:           SET_LEN_AND_RETURN_STR("MKCOL");
	case LI_HTTP_METHOD_PUT:             SET_LEN_AND_RETURN_STR("PUT");
	case LI_HTTP_METHOD_DELETE:          SET_LEN_AND_RETURN_STR("DELETE");
	case LI_HTTP_METHOD_COPY:            SET_LEN_AND_RETURN_STR("COPY");
	case LI_HTTP_METHOD_MOVE:            SET_LEN_AND_RETURN_STR("MOVE");
	case LI_HTTP_METHOD_PROPPATCH:       SET_LEN_AND_RETURN_STR("PROPPATCH");
	case LI_HTTP_METHOD_REPORT:          SET_LEN_AND_RETURN_STR("REPORT");
	case LI_HTTP_METHOD_CHECKOUT:        SET_LEN_AND_RETURN_STR("CHECKOUT");
	case LI_HTTP_METHOD_CHECKIN:         SET_LEN_AND_RETURN_STR("CHECKIN");
	case LI_HTTP_METHOD_VERSION_CONTROL: SET_LEN_AND_RETURN_STR("VERSION-CONTROL");
	case LI_HTTP_METHOD_UNCHECKOUT:      SET_LEN_AND_RETURN_STR("UNCHECKOUT");
	case LI_HTTP_METHOD_MKACTIVITY:      SET_LEN_AND_RETURN_STR("MKACTIVITY");
	case LI_HTTP_METHOD_MERGE:           SET_LEN_AND_RETURN_STR("MERGE");
	case LI_HTTP_METHOD_LOCK:            SET_LEN_AND_RETURN_STR("LOCK");
	case LI_HTTP_METHOD_UNLOCK:          SET_LEN_AND_RETURN_STR("UNLOCK");
	case LI_HTTP_METHOD_LABEL:           SET_LEN_AND_RETURN_STR("LABEL");
	case LI_HTTP_METHOD_CONNECT:         SET_LEN_AND_RETURN_STR("CONNECT");
	}

	*len = 0;
	return NULL;
}

#define METHOD_ENUM(x) do { if (0 == strncmp(#x, method_str, len)) return LI_HTTP_METHOD_##x; } while (0)
#define METHOD_ENUM2(x, y) do { if (0 == strncmp(x, method_str, len)) return LI_HTTP_METHOD_##y; } while (0)

liHttpMethod li_http_method_from_string(const gchar* method_str, gssize len) {
	switch (len) {
	case 3:
		METHOD_ENUM(GET);
		METHOD_ENUM(PUT);
		break;
	case 4:
		METHOD_ENUM(POST);
		METHOD_ENUM(HEAD);
		METHOD_ENUM(COPY);
		METHOD_ENUM(MOVE);
		METHOD_ENUM(LOCK);
		break;
	case 5:
		METHOD_ENUM(MKCOL);
		METHOD_ENUM(MERGE);
		METHOD_ENUM(LABEL);
		break;
	case 6:
		METHOD_ENUM(UNLOCK);
		METHOD_ENUM(DELETE);
		METHOD_ENUM(REPORT);
		break;
	case 7:
		METHOD_ENUM(OPTIONS);
		METHOD_ENUM(CONNECT);
		METHOD_ENUM(CHECKIN);
		break;
	case 8:
		METHOD_ENUM(PROPFIND);
		METHOD_ENUM(CHECKOUT);
		break;
	case 9:
		METHOD_ENUM(PROPPATCH);
		break;
	case 10:
		METHOD_ENUM(UNCHECKOUT);
		METHOD_ENUM(MKACTIVITY);
		break;
	case 15:
		METHOD_ENUM2("VERSION-CONTROL", VERSION_CONTROL);
		break;
	}

	return LI_HTTP_METHOD_UNSET;
}

#undef METHOD_ENUM
#undef METHOD_ENUM2

gchar *li_http_version_string(liHttpVersion method, guint *len) {
	switch (method) {
	case LI_HTTP_VERSION_1_1: SET_LEN_AND_RETURN_STR("HTTP/1.1");
	case LI_HTTP_VERSION_1_0: SET_LEN_AND_RETURN_STR("HTTP/1.0");
	case LI_HTTP_VERSION_UNSET: SET_LEN_AND_RETURN_STR("HTTP/??");
	}

	*len = 0;
	return NULL;
}

#undef SET_LEN_AND_RETURN_STR

void li_http_status_to_str(gint status_code, gchar status_str[]) {
	status_str[2] = status_code % 10 + '0';
	status_code /= 10;
	status_str[1] = status_code % 10 + '0';
	status_code /= 10;
	status_str[0] = status_code + '0';
}

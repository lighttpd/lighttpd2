
#include "utils.h"

#include <stdio.h>
#include <stdlib.h>

#include <fcntl.h>

void fatal(const gchar* msg) {
	fprintf(stderr, "%s\n", msg);
	abort();
}

void fd_init(int fd) {
#ifdef _WIN32
	int i = 1;
#endif
#ifdef FD_CLOEXEC
	/* close fd on exec (cgi) */
	fcntl(fd, F_SETFD, FD_CLOEXEC);
#endif
#ifdef O_NONBLOCK
	fcntl(fd, F_SETFL, O_NONBLOCK | O_RDWR);
#elif defined _WIN32
	ioctlsocket(fd, FIONBIO, &i);
#endif
}

void ev_io_add_events(struct ev_loop *loop, ev_io *watcher, int events) {
	if ((watcher->events & events) == events) return;
	ev_io_stop(loop, watcher);
	ev_io_set(watcher, watcher->fd, watcher->events | events);
	ev_io_start(loop, watcher);
}

void ev_io_rem_events(struct ev_loop *loop, ev_io *watcher, int events) {
	if (0 == (watcher->events & events)) return;
	ev_io_stop(loop, watcher);
	ev_io_set(watcher, watcher->fd, watcher->events & ~events);
	ev_io_start(loop, watcher);
}

void ev_io_set_events(struct ev_loop *loop, ev_io *watcher, int events) {
	if (events == (watcher->events & (EV_READ | EV_WRITE))) return;
	ev_io_stop(loop, watcher);
	ev_io_set(watcher, watcher->fd, (watcher->events & ~(EV_READ | EV_WRITE)) | events);
	ev_io_start(loop, watcher);
}


/* converts hex char (0-9, A-Z, a-z) to decimal.
 * returns -1 on invalid input.
 */
static int hex2int(unsigned char hex) {
	int res;
	if (hex >= 'A') { /* 'A' < 'a': hex >= 'A' --> hex >= 'a' */
		if (hex >= 'a') {
			res = hex - 'a' + 10;
		} else {
			res = hex - 'A' + 10;
		}
	} else {
		res = hex - '0';
	}
	if (res > 15)
		res = -1;

	return res;
}

void url_decode(GString *path) {
	char *src, *dst, c;
	src = dst = path->str;
	for ( ; *src; src++) {
		c = *src;
		if (c == '%') {
			if (src[1] && src[2]) {
				int a = hex2int(src[1]), b = hex2int(src[2]);
				if (a != -1 && b != -1) {
					c = (a << 4) | b;
					if (c < 32 || c == 127) c = '_';
					*(dst++) = c;
				}
				src += 2;
			} else {
				/* end of string */
				return;
			}
		} else {
			if (c < 32 || c == 127) c = '_';
			*(dst++) = c;
		}
	}
	g_string_set_size(path, dst - path->str);
}

/* Remove "/../", "//", "/./" parts from path.
 *
 * /blah/..         gets  /
 * /blah/../foo     gets  /foo
 * /abc/./xyz       gets  /abc/xyz
 * /abc//xyz        gets  /abc/xyz
 *
 * NOTE: src and dest can point to the same buffer, in which case
 *       the operation is performed in-place.
 */

void path_simplify(GString *path) {
	int toklen;
	char c, pre1;
	char *start, *slash, *walk, *out;
	unsigned short pre;

	if (path == NULL)
		return;

	walk  = start = out = slash = path->str;
	while (*walk == ' ') {
		walk++;
	}

	pre1 = *(walk++);
	c    = *(walk++);
	pre  = pre1;
	if (pre1 != '/') {
		pre = ('/' << 8) | pre1;
		*(out++) = '/';
	}
	*(out++) = pre1;

	if (pre1 == '\0') {
		g_string_set_size(path, out - start);
		return;
	}

	while (1) {
		if (c == '/' || c == '\0') {
			toklen = out - slash;
			if (toklen == 3 && pre == (('.' << 8) | '.')) {
				out = slash;
				if (out > start) {
					out--;
					while (out > start && *out != '/') {
						out--;
					}
				}

				if (c == '\0')
					out++;
			} else if (toklen == 1 || pre == (('/' << 8) | '.')) {
				out = slash;
				if (c == '\0')
					out++;
			}

			slash = out;
		}

		if (c == '\0')
			break;

		pre1 = c;
		pre  = (pre << 8) | pre1;
		c    = *walk;
		*out = pre1;

		out++;
		walk++;
	}

	g_string_set_size(path, out - start);
}

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
#undef SET_LEN_AND_RETURN_STR

void http_status_to_str(gint status_code, gchar status_str[]) {
	gint status_int;

	status_int = status_code;
	status_str[0] = status_int / 100;
	status_int -= 100 * status_str[0];
	status_str[1] = status_int / 10;
	status_int -= 10 * status_str[1];
	status_str[2] = status_int;
	status_str[0] += '0';
	status_str[1] += '0';
	status_str[2] += '0';
}


gchar counter_format(guint64 *count, guint factor) {
	gchar suffix = 0;

	if (*count >= factor) { *count /= factor; suffix = 'k';
		if (*count >= factor) { *count /= factor; suffix = 'm';
			if (*count >= factor) { *count /= factor; suffix = 'g';
				if (*count >= factor) { *count /= factor; suffix = 't';
					if (*count >= factor) { *count /= factor; suffix = 'p';
						if (*count >= factor) { *count /= factor; suffix = 'e'; }
					}
				}
			}
		}
	}

	return suffix;
}

gchar *ev_backend_string(guint backend) {
	switch (backend) {
		case EVBACKEND_SELECT:	return "select";
		case EVBACKEND_POLL:	return "poll";
		case EVBACKEND_EPOLL:	return "epoll";
		case EVBACKEND_KQUEUE:	return "kqueue";
		case EVBACKEND_DEVPOLL:	return "devpoll";
		case EVBACKEND_PORT:	return "port";
		default:				return "unknown";
	}
}


void string_destroy_notify(gpointer str) {
	g_string_free((GString*)str, TRUE);
}


guint hash_ipv4(gconstpointer key) {
	return *((guint*)key);
}

guint hash_ipv6(gconstpointer key) {
	guint *i = ((guint*)key);
	return i[0] ^ i[1] ^ i[2] ^ i[3];
}

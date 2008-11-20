

#include <lighttpd/base.h>
#include <lighttpd/plugin_core.h>

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
	status_str[2] = status_code % 10 + '0';
	status_code /= 10;
	status_str[1] = status_code % 10 + '0';
	status_code /= 10;
	status_str[0] = status_code + '0';
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

GString *counter_format2(guint64 count, counter_type t, gint accuracy) {
	GString *str = g_string_sized_new(64);

	if (t == COUNTER_TIME) {
		if (accuracy && count > (3600*24)) {
			g_string_append_printf(str, "%" G_GUINT64_FORMAT " day%s", count / (3600*24), (count / (3600*24)) > 1 ? "s":"");
			count %= 3600*24;
			accuracy--;
		}
		if (accuracy && count > 3600) {
			if (str->len)
				g_string_append_printf(str, " %" G_GUINT64_FORMAT " hour%s", count / 3600, (count / 3600) > 1 ? "s":"");
			else
				g_string_append_printf(str, "%" G_GUINT64_FORMAT " hour%s", count / 3600, (count / 3600) > 1 ? "s":"");
			count %= 3600;
			accuracy--;
		}
		if (accuracy && count > 60) {
			if (str->len)
				g_string_append_printf(str, " %" G_GUINT64_FORMAT " min", count / 60);
			else
				g_string_append_printf(str, "%" G_GUINT64_FORMAT " min", count / 60);
			count %= 60;
			accuracy--;
		}
		if (accuracy && (count || !str->len)) {
			if (str->len)
				g_string_append_printf(str, " %" G_GUINT64_FORMAT " s", count);
			else
				g_string_append_printf(str, "%" G_GUINT64_FORMAT " s", count);
		}
	} else if (t == COUNTER_UNITS) {
		if (accuracy && count > 1000000) {
			g_string_append_printf(str, "%" G_GUINT64_FORMAT " m", count / 1000000);
			count %= 1000000;
			accuracy--;
		}
		if (accuracy && count > 1000) {
			if (str->len)
				g_string_append_printf(str, " %" G_GUINT64_FORMAT " k", count / 1000);
			else
				g_string_append_printf(str, "%" G_GUINT64_FORMAT " k", count / 1000);
			count %= 1000;
			accuracy--;
		}
		if (accuracy && (count || !str->len)) {
			if (str->len)
				g_string_append_printf(str, " %" G_GUINT64_FORMAT, count);
			else
				g_string_append_printf(str, "%" G_GUINT64_FORMAT, count);
		}
	} else if (t == COUNTER_BYTES) {
		if (accuracy && count > (1024*1024*1024*G_GUINT64_CONSTANT(1024))) {
			g_string_append_printf(str, "%" G_GUINT64_FORMAT " TiB", count / (1024*1024*1024*G_GUINT64_CONSTANT(1024)));
			count %= (1024*1024*1024*G_GUINT64_CONSTANT(1024));
			accuracy--;
		}
		if (accuracy && count > (1024*1024*1024)) {
			if (str->len)
				g_string_append_printf(str, " %" G_GUINT64_FORMAT " GiB", count / (1024*1024*1024));
			else
				g_string_append_printf(str, "%" G_GUINT64_FORMAT " GiB", count / (1024*1024*1024));
			count %= (1024*1024*1024);
			accuracy--;
		}
		if (accuracy && count > (1024*1024)) {
			if (str->len)
				g_string_append_printf(str, " %" G_GUINT64_FORMAT " MiB", count / (1024*1024));
			else
				g_string_append_printf(str, "%" G_GUINT64_FORMAT " MiB", count / (1024*1024));
			count %= (1024*1024);
			accuracy--;
		}
		if (accuracy && count > 1024) {
			if (str->len)
				g_string_append_printf(str, " %" G_GUINT64_FORMAT " KiB", count / 1024);
			else
				g_string_append_printf(str, "%" G_GUINT64_FORMAT " KiB", count / 1024);
			count %= 1024;
			accuracy--;
		}
		if (accuracy && (count || !str->len)) {
			if (str->len)
				g_string_append_printf(str, " %" G_GUINT64_FORMAT " B", count);
			else
				g_string_append_printf(str, "%" G_GUINT64_FORMAT " B", count);
		}
	} else if (t == COUNTER_BITS) {
		if (accuracy && count > (1000*1000*1000)) {
			if (str->len)
				g_string_append_printf(str, " %" G_GUINT64_FORMAT " gb", count / (1000*1000*1000));
			else
				g_string_append_printf(str, "%" G_GUINT64_FORMAT " gbit", count / (1000*1000*1000));
			count %= (1024*1024*1024);
			accuracy--;
		}
		if (accuracy && count > (1024*1024)) {
			if (str->len)
				g_string_append_printf(str, " %" G_GUINT64_FORMAT " mbit", count / (1024*1024));
			else
				g_string_append_printf(str, "%" G_GUINT64_FORMAT " ", count / (1024*1024));
			count %= (1024*1024);
			accuracy--;
		}
		if (accuracy && count > 1024) {
			if (str->len)
				g_string_append_printf(str, " %" G_GUINT64_FORMAT " KiB", count / 1024);
			else
				g_string_append_printf(str, "%" G_GUINT64_FORMAT " KiB", count / 1024);
			count %= 1024;
			accuracy--;
		}
		if (accuracy && (count || !str->len)) {
			if (str->len)
				g_string_append_printf(str, " %" G_GUINT64_FORMAT " B", count);
			else
				g_string_append_printf(str, "%" G_GUINT64_FORMAT " B", count);
		}
	} else
		g_string_append_len(str, CONST_STR_LEN("unknown counter type"));

	return str;
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


GString *mimetype_get(vrequest *vr, GString *filename) {
	/* search in mime_types option for the first match */
	GArray *arr;

	if (!vr || !filename || !filename->len)
		return NULL;

	arr = CORE_OPTION(CORE_OPTION_MIME_TYPES).list;

	for (guint i = 0; i < arr->len; i++) {
		value *tuple = g_array_index(arr, value*, i);
		GString *ext = g_array_index(tuple->data.list, value*, 0)->data.string;
		if (ext->len > filename->len)
			continue;

		/* "" extension matches everything, used for default mimetype */
		if (!ext->len)
			return g_array_index(tuple->data.list, value*, 1)->data.string;

		gint k = filename->len - 1;
		gint j = ext->len - 1;
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


GString *sockaddr_to_string(sock_addr *saddr, GString *dest) {
	gchar *p;
	guint8 len = 0;
	guint8 tmp;
	guint8 tmplen;
	guint8 oct;

	switch (saddr->plain.sa_family) {
	case AF_INET:
		/* ipv4 */
		if (!dest)
			dest = g_string_sized_new(16);
		else
			g_string_set_size(dest, 16);

		p = dest->str;

		for (guint i = 0; i < 4; i++) {
			oct = ((guint8*)&saddr->ipv4.sin_addr.s_addr)[i];
			for (tmplen = 1, tmp = oct; tmp > 9; tmp /= 10)
				tmplen++;

			len += tmplen + 1;
			tmp = tmplen;

			p[tmplen] = '.';

			for (p += tmplen-1; tmplen; tmplen--) {
				*p = '0' + (oct % 10);
				p--;
				oct /= 10;
			}

			p += tmp + 2;
		}

		dest->str[len-1] = 0;
		dest->len = len-1;
		break;
#ifdef HAVE_IPV6
	case AF_INET6:
		/* ipv6 - not yet implemented with own function */
		if (!dest)
			dest = g_string_sized_new(INET6_ADDRSTRLEN);
		else
			g_string_set_size(dest, INET6_ADDRSTRLEN);

		inet_ntop(AF_INET6, saddr->ipv6.sin6_addr.s6_addr, dest->str, INET6_ADDRSTRLEN);
#endif
	}

	return dest;
}

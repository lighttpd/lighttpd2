

#include <lighttpd/base.h>
#include <lighttpd/plugin_core.h>

#include <stdio.h>
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


GString *counter_format(guint64 count, counter_type t, GString *dest) {
	guint64 rest;

	if (!dest)
		dest = g_string_sized_new(10);
	else
		g_string_truncate(dest, 0);

	switch (t) {
	case COUNTER_TIME:
		/* 123 days 12 hours 32 min 5 s */
		if (count > (3600*24)) {
			g_string_append_printf(dest, "%"G_GUINT64_FORMAT" days", count / (3600*24));
			count %= (3600*24);
		}
		if (count > 3600) {
			g_string_append_printf(dest, "%s%"G_GUINT64_FORMAT" hours", dest->len ? " ":"", count / 3600);
			count %= 3600;
		}
		if (count > 60) {
			g_string_append_printf(dest, "%s%"G_GUINT64_FORMAT" min", dest->len ? " ":"", count / 60);
			count %= 60;
		}
		if (count || !dest->len) {
			g_string_append_printf(dest, "%s%"G_GUINT64_FORMAT" s", dest->len ? " ":"", count);
		}
		break;
	case COUNTER_BYTES:
		/* B KB MB GB TB PB */
		if (count >> 50) {
			/* PB */
			rest = (((count >> 40) & 1023) * 100) / 1024;
			g_string_append_printf(dest, "%"G_GUINT64_FORMAT".%02"G_GUINT64_FORMAT" PB", count >> 50, rest);
		} else if (count >> 40) {
			/* TB */
			rest = (((count >> 30) & 1023) * 100) / 1024;
			g_string_append_printf(dest, "%"G_GUINT64_FORMAT".%02"G_GUINT64_FORMAT" TB", count >> 40, rest);
		} else if (count >> 30) {
			/* GB */
			rest = (((count >> 20) & 1023) * 100) / 1024;
			g_string_append_printf(dest, "%"G_GUINT64_FORMAT".%02"G_GUINT64_FORMAT" GB", count >> 30, rest);
		} else if (count >> 20) {
			/* MB */
			rest = (((count >> 10) & 1023) * 100) / 1024;
			g_string_append_printf(dest, "%"G_GUINT64_FORMAT".%02"G_GUINT64_FORMAT" MB", count >> 20, rest);
		} else if (count >> 10) {
			/* KB */
			rest = ((count & 1023) * 100) / 1024;
			g_string_append_printf(dest, "%"G_GUINT64_FORMAT".%02"G_GUINT64_FORMAT" KB", count >> 10, rest);
		} else {
			/* B */
			g_string_append_printf(dest, "%"G_GUINT64_FORMAT" B", count);
		}
		break;
	case COUNTER_UNITS:
		/* m k */
		if (count < 1000) {
			g_string_append_printf(dest, "%"G_GUINT64_FORMAT, count);
		} else if (count < 1000*1000) {
			g_string_append_printf(dest, "%"G_GUINT64_FORMAT".%02"G_GUINT64_FORMAT" k", count / (guint64)1000, (count % (guint64)1000) / 10);
		} else {
			g_string_append_printf(dest, "%"G_GUINT64_FORMAT".%02"G_GUINT64_FORMAT" m", count / (guint64)(1000*1000), (count % (guint64)(1000*1000)) / 10000);
		}
		break;
	}

	return dest;
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
	return *((guint*)key) * 2654435761;
}

guint hash_ipv6(gconstpointer key) {
	guint *i = ((guint*)key);
	return (i[0] ^ i[1] ^ i[2] ^ i[3]) * 2654435761;
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


GString *sockaddr_to_string(sockaddr_t addr, GString *dest, gboolean showport) {
	gchar *p;
	guint8 len = 0;
	guint8 tmp;
	guint8 tmplen;
	guint8 oct;
	sock_addr *saddr = addr.addr;

	switch (saddr->plain.sa_family) {
	case AF_INET:
		/* ipv4 */
		if (!dest)
			dest = g_string_sized_new(16+6);
		else
			g_string_set_size(dest, 16+6);

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
		if (showport) g_string_append_printf(dest, ":%u", (unsigned int) ntohs(saddr->ipv4.sin_port));
		break;
#ifdef HAVE_IPV6
	case AF_INET6:
		/* ipv6 - not yet implemented with own function */
		if (!dest)
			dest = g_string_sized_new(INET6_ADDRSTRLEN+6);

		ipv6_tostring(dest, saddr->ipv6.sin6_addr.s6_addr);
		if (showport) g_string_append_printf(dest, ":%u", (unsigned int) ntohs(saddr->ipv6.sin6_port));
		break;
#endif
#ifdef HAVE_SYS_UN_H
	case AF_UNIX:
		if (!dest)
			dest = g_string_sized_new(0);
		else
			g_string_truncate(dest, 0);
		g_string_append_len(dest, CONST_STR_LEN("unix:"));
		g_string_append(dest, saddr->un.sun_path);
		break;
#endif
	default:
		if (!dest)
			dest = g_string_new_len(CONST_STR_LEN("unknown sockaddr family"));
		else
			l_g_string_assign_len(dest, CONST_STR_LEN("unknown sockaddr family"));
	}

	return dest;
}

sockaddr_t sockaddr_from_string(GString *str, guint tcp_default_port) {
	guint32 ipv4;
#ifdef HAVE_IPV6
	guint8 ipv6[16];
#endif
	guint16 port = tcp_default_port;
	sockaddr_t saddr = { 0, NULL };

#ifdef HAVE_SYS_UN_H
	if (0 == strncmp(str->str, "unix:/", 6)) {
		saddr.len = str->len + 1 - 5 + sizeof(saddr.addr->un.sun_family);
		saddr.addr = (sock_addr*) g_slice_alloc0(saddr.len);
		saddr.addr->un.sun_family = AF_UNIX;
		strcpy(saddr.addr->un.sun_path, str->str + 5);
	} else
#endif
	if (parse_ipv4(str->str, &ipv4, NULL, &port)) {
		if (!port) return saddr;
		saddr.len = sizeof(struct sockaddr_in);
		saddr.addr = (sock_addr*) g_slice_alloc0(saddr.len);
		saddr.addr->ipv4.sin_family = AF_INET;
		saddr.addr->ipv4.sin_addr.s_addr = ipv4;
		saddr.addr->ipv4.sin_port = htons(port);
#ifdef HAVE_IPV6
	} else
	if (parse_ipv6(str->str, ipv6, NULL, &port)) {
		if (!port) return saddr;
		saddr.len = sizeof(struct sockaddr_in6);
		saddr.addr = (sock_addr*) g_slice_alloc0(saddr.len);
		saddr.addr->ipv6.sin6_family = AF_INET6;
		memcpy(&saddr.addr->ipv6.sin6_addr, ipv6, 16);
		saddr.addr->ipv6.sin6_port = htons(port);
#endif
	}
	return saddr;
}

sockaddr_t sockaddr_local_from_socket(gint fd) {
	socklen_t l = 0;
	static struct sockaddr sa;
	struct sockaddr_t saddr = { 0, NULL };

	if (-1 == getsockname(fd, &sa, &l)) {
		return saddr;
	}

	saddr.addr = (sock_addr*) g_slice_alloc0(l);
	saddr.len = l;
	getsockname(fd, (struct sockaddr*) saddr.addr, &l);

	return saddr;
}

sockaddr_t sockaddr_remote_from_socket(gint fd) {
	socklen_t l = 0;
	static struct sockaddr sa;
	struct sockaddr_t saddr = { 0, NULL };

	if (-1 == getpeername(fd, &sa, &l)) {
		return saddr;
	}

	saddr.addr = (sock_addr*) g_slice_alloc0(l);
	saddr.len = l;
	getpeername(fd, (struct sockaddr*) saddr.addr, &l);

	return saddr;
}

void sockaddr_clear(sockaddr_t *saddr) {
	if (saddr->addr) g_slice_free1(saddr->len, saddr->addr);
	saddr->addr = NULL;
	saddr->len = 0;
}

/* unused */
void gstring_replace_char_with_str_len(GString *gstr, gchar c, gchar *str, guint len) {
	for (guint i = 0; i < gstr->len; i++) {
		if (gstr->str[i] == c) {
			/* char found, replace */
			gstr->str[i] = str[0];
			if (len > 1)
				g_string_insert_len(gstr, i, &str[1], len-1);
			i += len - 1;
		}
	}
}

gboolean l_g_strncase_equal(GString *str, const gchar *s, guint len) {
	if (str->len != len) return FALSE;
	return 0 == g_ascii_strncasecmp(str->str, s, len);
}

gboolean l_g_string_suffix(GString *str, const gchar *s, gsize len) {
	if (str->len < len)
		return FALSE;

	return (strcmp(str->str + str->len - len, s) == 0);
}

gboolean l_g_string_prefix(GString *str, const gchar *s, gsize len) {
	if (str->len < len)
		return FALSE;

	return (strncmp(str->str, s, len) == 0);
}

GString *l_g_string_assign_len(GString *string, const gchar *val, gssize len) {
	g_string_truncate(string, 0);
	g_string_append_len(string, val, len);
	return string;
}

void l_g_string_append_int(GString *dest, gint64 v) {
	gchar *buf, *end, swap;
	gsize len;
	guint64 val;

	len = dest->len + 1;
	g_string_set_size(dest, dest->len + 21);
	buf = dest->str + len - 1;

	if (v < 0) {
		len++;
		*(buf++) = '-';
		/* -v maybe < 0 for signed types, so just cast it to unsigned to get the correct value */
		val = -v;
	} else {
		val = v;
	}

	end = buf;
	while (val > 9) {
		*(end++) = '0' + (val % 10);
		val = val / 10;
	}
	*(end) = '0' + val;
	*(end + 1) = '\0';
	len += end - buf;

	while (buf < end) {
		swap = *end;
		*end = *buf;
		*buf = swap;

		buf++;
		end--;
	}

	dest->len = len;
}

/* http://womble.decadentplace.org.uk/readdir_r-advisory.html */
gsize dirent_buf_size(DIR * dirp) {
	glong name_max;
 	gsize name_end;

#	if !defined(HAVE_DIRFD)
		UNUSED(dirp);
#	endif

#	if defined(HAVE_FPATHCONF) && defined(HAVE_DIRFD) && defined(_PC_NAME_MAX)
		name_max = fpathconf(dirfd(dirp), _PC_NAME_MAX);
		if (name_max == -1)
#			if defined(NAME_MAX)
				name_max = (NAME_MAX > 255) ? NAME_MAX : 255;
#			else
				return (gsize)(-1);
#			endif
#	else
#		if defined(NAME_MAX)
			name_max = (NAME_MAX > 255) ? NAME_MAX : 255;
#		else
#			error "buffer size for readdir_r cannot be determined"
#		endif
#	endif

    name_end = (gsize)offsetof(struct dirent, d_name) + name_max + 1;

    return (name_end > sizeof(struct dirent) ? name_end : sizeof(struct dirent));
}


#define STRNCMP_TEST(_x) strncmp(c, CONST_STR_LEN(_x)) == 0
guint cond_lvalue_from_str(gchar *c, cond_lvalue_t *lval) {
	gchar *c_orig = c;

	if (STRNCMP_TEST("req")) {
		c += sizeof("req")-1;

		if (*c == '.')
			c++;
		else if (strncmp(c, CONST_STR_LEN("uest.")) == 0)
			c += sizeof("uest.")-1;
		else
			return 0;

		if (STRNCMP_TEST("localip")) {
			*lval = COMP_REQUEST_LOCALIP;
			return c - c_orig + sizeof("localip")-1;
		} else if (STRNCMP_TEST("remoteip")) {
			*lval = COMP_REQUEST_REMOTEIP;
			return c - c_orig + sizeof("remoteip")-1;
		} else if (STRNCMP_TEST("path")) {
			*lval = COMP_REQUEST_PATH;
			return c - c_orig + sizeof("path")-1;
		} else if (STRNCMP_TEST("host")) {
			*lval = COMP_REQUEST_HOST;
			return c - c_orig + sizeof("host")-1;
		} else if (STRNCMP_TEST("scheme")) {
			*lval = COMP_REQUEST_SCHEME;
			return c - c_orig + sizeof("scheme")-1;
		} else if (STRNCMP_TEST("query")) {
			*lval = COMP_REQUEST_QUERY_STRING;
			return c - c_orig + sizeof("query")-1;
		} else if (STRNCMP_TEST("method")) {
			*lval = COMP_REQUEST_METHOD;
			return c - c_orig + sizeof("method")-1;
		} else if (STRNCMP_TEST("content_length")) {
			*lval = COMP_REQUEST_CONTENT_LENGTH;
			return c - c_orig + sizeof("content_length")-1;
		}
	} else if (STRNCMP_TEST("phys")) {
		c += sizeof("phys")-1;

		if (*c == '.')
			c++;
		else if (STRNCMP_TEST("ical."))
			c += sizeof("ical.")-1;
		else
			return 0;

		if (STRNCMP_TEST("path")) {
			*lval = COMP_PHYSICAL_PATH;
			return c - c_orig + sizeof("path")-1;
		} else if (STRNCMP_TEST("exists")) {
			*lval = COMP_PHYSICAL_PATH_EXISTS;
			return c - c_orig + sizeof("exists")-1;
		} else if (STRNCMP_TEST("size")) {
			*lval = COMP_PHYSICAL_SIZE;
			return c - c_orig + sizeof("size")-1;
		} else if (STRNCMP_TEST("sidir")) {
			*lval = COMP_PHYSICAL_ISDIR;
			return c - c_orig + sizeof("sidir")-1;
		} else if (STRNCMP_TEST("isfile")) {
			*lval = COMP_PHYSICAL_ISFILE;
			return sizeof("isfile")-1;
		}
	}

	return 0;
}
#undef STRNCMP_TEST
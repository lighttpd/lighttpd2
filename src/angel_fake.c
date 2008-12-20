
#include <lighttpd/base.h>
#include <lighttpd/angel.h>

/* listen to a socket */
int angel_fake_listen(server *srv, GString *str) {
	guint32 ipv4;
#ifdef HAVE_IPV6
	guint8 ipv6[16];
#endif
	GString *ipstr;
	guint16 port = 80;

	if (str->str[0] == '[') {
		/* ipv6 with port */
		gchar *pos = g_strrstr(str->str, "]");
		if (NULL == pos) {
			ERROR(srv, "%s", "listen: bogus ipv6 format");
			return -1;
		}
		if (pos[1] == ':') {
			port = atoi(&pos[2]);
		}
		ipstr = g_string_new_len(&str->str[1], pos - &str->str[1]);
	} else {
		/* no brackets, search for :port */
		gchar *pos = g_strrstr(str->str, ":");
		if (NULL != pos) {
			ipstr = g_string_new_len(str->str, pos - str->str);
			port = atoi(&pos[1]);
		} else {
			/* no port, just plain ipv4 or ipv6 address */
			ipstr = g_string_new_len(GSTR_LEN(str));
		}
	}

	if (parse_ipv4(ipstr->str, &ipv4, NULL)) {
		int s, v;
		struct sockaddr_in addr;
		memset(&addr, 0, sizeof(addr));
		addr.sin_family = AF_INET;
		addr.sin_addr.s_addr = ipv4;
		addr.sin_port = htons(port);
		g_string_free(ipstr, TRUE);
		if (-1 == (s = socket(AF_INET, SOCK_STREAM, 0))) {
			ERROR(srv, "Couldn't open socket: %s", g_strerror(errno));
			return -1;
		}
		v = 1;
		if (-1 == setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &v, sizeof(v))) {
			close(s);
			ERROR(srv, "Couldn't setsockopt(SO_REUSEADDR) to '%s': %s", inet_ntoa(*(struct in_addr*)&ipv4), g_strerror(errno));
			return -1;
		}
		if (-1 == bind(s, (struct sockaddr*)&addr, sizeof(addr))) {
			close(s);
			ERROR(srv, "Couldn't bind socket to '%s': %s", inet_ntoa(*(struct in_addr*)&ipv4), g_strerror(errno));
			return -1;
		}
		if (-1 == listen(s, 1000)) {
			close(s);
			ERROR(srv, "Couldn't listen on '%s': %s", inet_ntoa(*(struct in_addr*)&ipv4), g_strerror(errno));
			return -1;
		}
		TRACE(srv, "listen to ipv4: '%s' port: %d", inet_ntoa(*(struct in_addr*)&ipv4), port);
		return s;
#ifdef HAVE_IPV6
	} else if (parse_ipv6(ipstr->str, ipv6, NULL)) {
		/* TODO: IPv6 */
		g_string_free(ipstr, TRUE);
		ERROR(srv, "%s", "IPv6 not supported yet");
		return -1;
#endif
	} else {
		ERROR(srv, "Invalid ip: '%s'", ipstr->str);
		g_string_free(ipstr, TRUE);
		return -1;
	}
}

/* send log messages while startup to angel */
gboolean angel_fake_log(server *srv, GString *str) {
	const char *buf;
	guint len;
	ssize_t written;
	UNUSED(srv);

	g_string_prepend(str, "fake: ");
	buf = str->str;
	len = str->len;

	while (len > 0) {
		written = write(2, buf, len);
		if (written < 0) {
			g_string_free(str, TRUE);
			return FALSE;
		}
		len -= written;
		buf += written;
	}
	g_string_free(str, TRUE);
	return TRUE;
}

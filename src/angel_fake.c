
#include <lighttpd/base.h>
#include <lighttpd/angel.h>

/* listen to a socket */
int angel_fake_listen(server *srv, GString *str) {
	guint32 ipv4;
#ifdef HAVE_IPV6
	guint8 ipv6[16];
#endif
	guint16 port = 80;

	if (parse_ipv4(str->str, &ipv4, NULL, &port)) {
		int s, v;
		struct sockaddr_in addr;
		memset(&addr, 0, sizeof(addr));
		addr.sin_family = AF_INET;
		addr.sin_addr.s_addr = ipv4;
		addr.sin_port = htons(port);
		if (-1 == (s = socket(AF_INET, SOCK_STREAM, 0))) {
			ERROR(srv, "Couldn't open socket: %s", g_strerror(errno));
			return -1;
		}
		v = 1;
		if (-1 == setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &v, sizeof(v))) {
			close(s);
			ERROR(srv, "Couldn't setsockopt(SO_REUSEADDR): %s", g_strerror(errno));
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
	} else if (parse_ipv6(str->str, ipv6, NULL, &port)) {
		GString *ipv6_str = g_string_sized_new(0);
		int s, v;
		struct sockaddr_in6 addr;
		ipv6_tostring(ipv6_str, ipv6);
		
		memset(&addr, 0, sizeof(addr));
		addr.sin6_family = AF_INET6;
		memcpy(&addr.sin6_addr, ipv6, 16);
		addr.sin6_port = htons(port);
		if (-1 == (s = socket(AF_INET6, SOCK_STREAM, 0))) {
			ERROR(srv, "Couldn't open socket: %s", g_strerror(errno));
			g_string_free(ipv6_str, TRUE);
			return -1;
		}
		v = 1;
		if (-1 == setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &v, sizeof(v))) {
			close(s);
			ERROR(srv, "Couldn't setsockopt(SO_REUSEADDR): %s", g_strerror(errno));
			g_string_free(ipv6_str, TRUE);
			return -1;
		}
		if (-1 == setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, &v, sizeof(v))) {
			close(s);
			ERROR(srv, "Couldn't setsockopt(IPV6_V6ONLY): %s", g_strerror(errno));
			g_string_free(ipv6_str, TRUE);
			return -1;
		}
		if (-1 == bind(s, (struct sockaddr*)&addr, sizeof(addr))) {
			close(s);
			ERROR(srv, "Couldn't bind socket to '%s': %s", ipv6_str->str, g_strerror(errno));
			g_string_free(ipv6_str, TRUE);
			return -1;
		}
		if (-1 == listen(s, 1000)) {
			close(s);
			ERROR(srv, "Couldn't listen on '%s': %s", ipv6_str->str, g_strerror(errno));
			g_string_free(ipv6_str, TRUE);
			return -1;
		}
		TRACE(srv, "listen to ipv6: '%s' port: %d", ipv6_str->str, port);
		g_string_free(ipv6_str, TRUE);
		return s;
#endif
	} else {
		ERROR(srv, "Invalid ip: '%s'", str->str);
		return -1;
	}
}

/* print log messages during startup to stderr */
gboolean angel_fake_log(server *srv, GString *str) {
	const char *buf;
	guint len;
	ssize_t written;
	UNUSED(srv);

	/* g_string_prepend(str, "fake: "); */
	buf = str->str;
	len = str->len;

	while (len > 0) {
		written = write(2, buf, len);
		if (written < 0) {
			switch (errno) {
			case EAGAIN:
			case EINTR:
				continue;
			}
			g_string_free(str, TRUE);
			return FALSE;
		}
		len -= written;
		buf += written;
	}
	g_string_free(str, TRUE);
	return TRUE;
}

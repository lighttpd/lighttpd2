
#include <lighttpd/base.h>
#include <lighttpd/angel.h>
#include <lighttpd/ip_parsers.h>

#include <fcntl.h>

/* listen to a socket */
int li_angel_fake_listen(liServer *srv, GString *str) {
	liSocketAddress addr = li_sockaddr_from_string(str, 80);
	liSockAddrPtr saddr_up = addr.addr_up;
	GString *tmpstr;
	int s, v;

	if (NULL == saddr_up.raw) {
		ERROR(srv, "Invalid socket address: '%s'", str->str);
		return -1;
	}

	tmpstr = li_sockaddr_to_string(addr, NULL, TRUE);

	switch (saddr_up.plain->sa_family) {
#ifdef HAVE_SYS_UN_H
	case AF_UNIX:
		if (-1 == unlink(saddr_up.un->sun_path)) {
			switch (errno) {
			case ENOENT:
				break;
			default:
				ERROR(srv, "removing old socket '%s' failed: %s\n", str->str, g_strerror(errno));
				goto error;
			}
		}
		if (-1 == (s = socket(saddr_up.plain->sa_family, SOCK_STREAM, 0))) {
			ERROR(srv, "Couldn't open socket: %s", g_strerror(errno));
			goto error;
		}
		if (-1 == bind(s, saddr_up.plain, addr.len)) {
			ERROR(srv, "Couldn't bind socket to '%s': %s", tmpstr->str, g_strerror(errno));
			close(s);
			goto error;
		}
		if (-1 == listen(s, 1000)) {
			ERROR(srv, "Couldn't listen on '%s': %s", tmpstr->str, g_strerror(errno));
			close(s);
			goto error;
		}
		DEBUG(srv, "listen to unix socket: '%s'", tmpstr->str);
		break;
#endif
	case AF_INET:
#ifdef HAVE_IPV6
	case AF_INET6:
#endif
		if (-1 == (s = socket(saddr_up.plain->sa_family, SOCK_STREAM, 0))) {
			ERROR(srv, "Couldn't open socket: %s", g_strerror(errno));
			goto error;
		}
		v = 1;
		if (-1 == setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &v, sizeof(v))) {
			ERROR(srv, "Couldn't setsockopt(SO_REUSEADDR): %s", g_strerror(errno));
			close(s);
			goto error;
		}
#ifdef HAVE_IPV6
		if (AF_INET6 == saddr_up.plain->sa_family && -1 == setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, &v, sizeof(v))) {
			ERROR(srv, "Couldn't setsockopt(IPV6_V6ONLY): %s", g_strerror(errno));
			close(s);
			goto error;
		}
#endif
		if (-1 == bind(s, saddr_up.plain, addr.len)) {
			ERROR(srv, "Couldn't bind socket to '%s': %s", tmpstr->str, g_strerror(errno));
			close(s);
			goto error;
		}
#ifdef TCP_FASTOPEN
		v = 1000;
		setsockopt(s, IPPROTO_TCP, TCP_FASTOPEN, &v, sizeof(v));
#endif
		if (-1 == listen(s, 1000)) {
			ERROR(srv, "Couldn't listen on '%s': %s", tmpstr->str, g_strerror(errno));
			close(s);
			goto error;
		}
#ifdef HAVE_IPV6
		if (AF_INET6 == saddr_up.plain->sa_family) {
			DEBUG(srv, "listen to ipv6: '%s'", tmpstr->str);
		} else
#endif
		{
			DEBUG(srv, "listen to ipv4: '%s'", tmpstr->str);
		}
		break;
	default:
		ERROR(srv, "Unknown address family for '%s'", tmpstr->str);
		goto error;
	}

	g_string_free(tmpstr, TRUE);
	li_sockaddr_clear(&addr);
	return s;

error:
	g_string_free(tmpstr, TRUE);
	li_sockaddr_clear(&addr);
	return -1;
}

/* print log messages during startup to stderr */
gboolean li_angel_fake_log(liServer *srv, GString *str) {
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

int li_angel_fake_log_open_file(liServer *srv, GString *filename) {
	int fd;

	fd = open(filename->str, O_RDWR | O_CREAT | O_APPEND, 0660);
	if (-1 == fd) {
		ERROR(srv, "failed to open log file '%s': %s", filename->str, g_strerror(errno));
	}

	return fd;
}

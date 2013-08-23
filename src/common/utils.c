
#include <lighttpd/utils.h>
#include <lighttpd/ip_parsers.h>

#include <stdio.h>
#include <fcntl.h>

#ifdef HAVE_CRYPT_H
# include <crypt.h>
#endif

/* for send/li_receive_fd */
union fdmsg {
  struct cmsghdr h;
  gchar buf[1000];
};


void li_fatal(const gchar* msg) {
	fprintf(stderr, "%s\n", msg);
	abort();
}

void li_fd_no_block(int fd) {
#ifdef O_NONBLOCK
	fcntl(fd, F_SETFL, O_NONBLOCK | O_RDWR);
#elif defined _WIN32
	int i = 1;
	ioctlsocket(fd, FIONBIO, &i);
#else
#error No way found to set non-blocking mode for fds.
#endif
}

void li_fd_block(int fd) {
#ifdef O_NONBLOCK
	fcntl(fd, F_SETFL, O_RDWR);
#elif defined _WIN32
	int i = 0;
	ioctlsocket(fd, FIONBIO, &i);
#else
#error No way found to set blocking mode for fds.
#endif
}

void li_fd_init(int fd) {
#ifdef FD_CLOEXEC
	/* close fd on exec (cgi) */
	fcntl(fd, F_SETFD, FD_CLOEXEC);
#endif
	li_fd_no_block(fd);
}

#if 0
#ifndef _WIN32
int li_send_fd(int s, int fd) { /* write fd to unix socket s */
	for ( ;; ) {
		if (-1 == ioctl(s, I_SENDFD, fd)) {
			switch (errno) {
			case EINTR: break;
			case EAGAIN: return -2;
			default: return -1;
			}
		} else {
			return 0;
		}
	}
}

int li_receive_fd(int s, int *fd) { /* read fd from unix socket s */
	struct strrecvfd res;
	memset(&res, 0, sizeof(res));
	for ( ;; ) {
		if (-1 == ioctl(s, I_RECVFD, &res)) {
			switch (errno) {
			case EINTR: break;
			case EAGAIN: return -2;
			default: return -1;
			}
		} else {
			*fd = res.fd;
		}
	}
}
#endif
#endif

gint li_send_fd(gint s, gint fd) { /* write fd to unix socket s */
	struct msghdr msg;
	struct iovec  iov;
#ifdef CMSG_FIRSTHDR
	struct cmsghdr *cmsg;
# ifndef CMSG_SPACE
#  define CMSG_SPACE(x) x+100
# endif
	gchar buf[CMSG_SPACE(sizeof(gint))];
#endif

	memset(&msg, 0, sizeof(msg));
	memset(&iov, 0, sizeof(iov));

	iov.iov_len = 1;
	iov.iov_base = "x";
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	msg.msg_name = 0;
	msg.msg_namelen = 0;
#ifdef CMSG_FIRSTHDR
	msg.msg_control = buf;
	msg.msg_controllen = sizeof(buf);
	cmsg = CMSG_FIRSTHDR(&msg);
	cmsg->cmsg_level = SOL_SOCKET;
	cmsg->cmsg_type = SCM_RIGHTS;
#ifndef CMSG_LEN
#define CMSG_LEN(x) x
#endif
	cmsg->cmsg_len = CMSG_LEN(sizeof(gint));
	msg.msg_controllen = cmsg->cmsg_len;
	memcpy(CMSG_DATA(cmsg), &fd, sizeof(gint));
#else
	msg.msg_accrights = (gchar*)&fd;
	msg.msg_accrightslen = sizeof(fd);
#endif

	for (;;) {
		if (sendmsg(s, &msg, 0) < 0) {
			switch (errno) {
			case EINTR: continue;
#if EAGAIN != EWOULDBLOCK
			case EWOULDBLOCK:
#endif
			case EAGAIN: return -2;
			default: return -1;
			}
		}

		break;
	}

	return 0;
}


gint li_receive_fd(gint s, gint *fd) { /* read fd from unix socket s */
	struct iovec iov;
	struct msghdr msg;
	ssize_t r;
#ifdef CMSG_FIRSTHDR
	union fdmsg cmsg;
	struct cmsghdr* h;
#endif
	gint _fd;
	gchar x = '\0';
	gchar name[100];

	memset(&msg, 0, sizeof(msg));
	memset(&iov, 0, sizeof(iov));

	iov.iov_base = &x;
	iov.iov_len = 1;
	msg.msg_name = name;
	msg.msg_namelen = 100;
#ifdef CMSG_FIRSTHDR
	msg.msg_control = cmsg.buf;
	msg.msg_controllen = sizeof(union fdmsg);
#else
	msg.msg_accrights = (gchar*)&_fd;
	msg.msg_accrightslen = sizeof(_fd);
#endif
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
#ifdef CMSG_FIRSTHDR
	msg.msg_flags = 0;
	h = CMSG_FIRSTHDR(&msg);
#ifndef CMSG_LEN
	#define CMSG_LEN(x) x
#endif
	h->cmsg_len = CMSG_LEN(sizeof(gint));
	h->cmsg_level = SOL_SOCKET;
	h->cmsg_type = SCM_RIGHTS;
	_fd = -1;
	memcpy(CMSG_DATA(h), &_fd, sizeof(gint));
#endif

	for (;;) {
		if (-1 == (r = recvmsg(s, &msg, 0))) {
			switch (errno) {
			case EINTR: continue;
#if EAGAIN != EWOULDBLOCK
			case EWOULDBLOCK:
#endif
			case EAGAIN: return -2;
			default: return -1;
			}
		}

		break;
	}

	if (1 != r || x != 'x') {
#ifdef EPROTO
		errno = EPROTO;
#else
		errno = EINVAL;
#endif
		return -1;
	}

#ifdef CMSG_FIRSTHDR
	h = CMSG_FIRSTHDR(&msg);

	if (!h || h->cmsg_len != CMSG_LEN(sizeof(gint)) || h->cmsg_level != SOL_SOCKET || h->cmsg_type != SCM_RIGHTS) {
#ifdef EPROTO
		errno = EPROTO;
#else
		errno = EINVAL;
#endif
		return -1;
	}

	memcpy(fd, CMSG_DATA(h), sizeof(gint));

	return 0;
#else
	if (msg.msg_accrightslen != sizeof(fd))
		return -1;

	*fd = _fd;

	return 0;
#endif
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

void li_url_decode(GString *path) {
	unsigned char *src, *dst, c;
	src = dst = (unsigned char*) path->str;
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
	g_string_set_size(path, dst - (unsigned char*) path->str);
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

void li_path_simplify(GString *path) {
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


gboolean li_querystring_find(const GString *querystring, const gchar *key, const guint key_len, gchar **val, guint *val_len) {
	gchar delim = '\0';
	gchar *end = querystring->str + querystring->len;
	gchar *start = querystring->str;
	gchar *c;

	/* search for key */
	for (c = querystring->str; c != end; c++) {
		if ((*c == '&' || *c == ';') && delim == '\0')
			delim = *c;

		if (*c == '=' || (*c == delim && delim != '\0')) {
			if ((c - start) == (gint)key_len && memcmp(start, key, key_len) == 0) {
				/* key found */
				c++;
				*val = c;

				/* get length of val */
				for (; c != end; c++) {
					if ((*c == '&' || *c == ';') && (delim == '\0' || *c == delim))
						break;
				}

				*val_len = c - *val;
				return TRUE;
			}

			start = c + 1;
		}
	}

	return FALSE;
}


GString *li_counter_format(guint64 count, liCounterType t, GString *dest) {
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


void li_string_destroy_notify(gpointer str) {
	g_string_free((GString*)str, TRUE);
}

guint li_hash_binary_len(gconstpointer data, gsize len) {
	GString str = li_const_gstring(data, len);
	return g_string_hash(&str);
}

guint li_hash_ipv4(gconstpointer key) {
	return *((guint*)key) * 2654435761;
}

guint li_hash_ipv6(gconstpointer key) {
	return li_hash_binary_len(key, 16);
}

guint li_hash_sockaddr(gconstpointer key) {
	const liSocketAddress *addr = key;
	return li_hash_binary_len(addr->addr, addr->len);
}
gboolean li_equal_sockaddr(gconstpointer key1, gconstpointer key2) {
	const liSocketAddress *addr1 = key1, *addr2 = key2;
	if (addr1->len != addr2->len) return FALSE;
	if (addr1->addr == addr2->addr) return TRUE;
	if (!addr1->addr || !addr2->addr) return FALSE;
	return 0 == memcmp(addr1->addr, addr2->addr, addr1->len);
}

GString *li_sockaddr_to_string(liSocketAddress addr, GString *dest, gboolean showport) {
	gchar *p;
	guint8 len = 0;
	guint8 tmp;
	guint8 tmplen;
	guint8 oct;
	liSockAddr *saddr = addr.addr;
	guint i;

	if (!saddr) {
		li_string_assign_len(dest, CONST_STR_LEN("<null>"));
		return dest;
	}

	switch (saddr->plain.sa_family) {
	case AF_INET:
		/* ipv4 */
		if (!dest)
			dest = g_string_sized_new(16+6);
		else
			g_string_set_size(dest, 16+6);

		p = dest->str;

		for (i = 0; i < 4; i++) {
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

		li_ipv6_tostring(dest, saddr->ipv6.sin6_addr.s6_addr);
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
			li_string_assign_len(dest, CONST_STR_LEN("unknown sockaddr family"));
	}

	return dest;
}

liSocketAddress li_sockaddr_from_string(const GString *str, guint tcp_default_port) {
	guint32 ipv4;
#ifdef HAVE_IPV6
	guint8 ipv6[16];
#endif
	guint16 port;
	liSocketAddress saddr = { 0, NULL };

#ifdef HAVE_SYS_UN_H
	if (0 == strncmp(str->str, "unix:/", 6)) {
		/* try to support larger unix socket names than what fits in the default sockaddr_un struct */
		saddr.len = str->len + 1 - 5 + sizeof(saddr.addr->un) - sizeof(saddr.addr->un.sun_path);
		if (saddr.len < sizeof(saddr.addr->un)) saddr.len = sizeof(saddr.addr->un);
		saddr.addr = (liSockAddr*) g_slice_alloc0(saddr.len);
		saddr.addr->un.sun_family = AF_UNIX;
		strcpy(saddr.addr->un.sun_path, str->str + 5);
	} else
#endif
	if (li_parse_ipv4(str->str, &ipv4, NULL, &port)) {
		if (!port) port = tcp_default_port;
		saddr.len = sizeof(struct sockaddr_in);
		saddr.addr = (liSockAddr*) g_slice_alloc0(saddr.len);
		saddr.addr->ipv4.sin_family = AF_INET;
		saddr.addr->ipv4.sin_addr.s_addr = ipv4;
		saddr.addr->ipv4.sin_port = htons(port);
#ifdef HAVE_IPV6
	} else
	if (li_parse_ipv6(str->str, ipv6, NULL, &port)) {
		if (!port) port = tcp_default_port;
		saddr.len = sizeof(struct sockaddr_in6);
		saddr.addr = (liSockAddr*) g_slice_alloc0(saddr.len);
		saddr.addr->ipv6.sin6_family = AF_INET6;
		memcpy(&saddr.addr->ipv6.sin6_addr, ipv6, 16);
		saddr.addr->ipv6.sin6_port = htons(port);
#endif
	}
	return saddr;
}

liSocketAddress li_sockaddr_local_from_socket(gint fd) {
	liSockAddr sa;
	socklen_t l = sizeof(sa);
	liSocketAddress saddr = { 0, NULL };

	if (-1 == getsockname(fd, &sa.plain, &l)) {
		return saddr;
	}

	saddr.addr = (liSockAddr*) g_slice_alloc0(l);
	saddr.len = l;
	if (l <= sizeof(sa)) {
		memcpy(saddr.addr, &sa.plain, l);
	} else {
		getsockname(fd, (struct sockaddr*) saddr.addr, &l);
	}

	return saddr;
}

liSocketAddress li_sockaddr_remote_from_socket(gint fd) {
	liSockAddr sa;
	socklen_t l = sizeof(sa);
	liSocketAddress saddr = { 0, NULL };

	if (-1 == getpeername(fd, &sa.plain, &l)) {
		return saddr;
	}

	saddr.addr = (liSockAddr*) g_slice_alloc0(l);
	saddr.len = l;
	if (l <= sizeof(sa)) {
		memcpy(saddr.addr, &sa.plain, l);
	} else {
		getpeername(fd, (struct sockaddr*) saddr.addr, &l);
	}

	return saddr;
}

void li_sockaddr_clear(liSocketAddress *saddr) {
	if (saddr->addr) g_slice_free1(saddr->len, saddr->addr);
	saddr->addr = NULL;
	saddr->len = 0;
}

liSocketAddress li_sockaddr_dup(liSocketAddress saddr) {
	liSocketAddress naddr = { 0, NULL };
	naddr.addr = (liSockAddr*) g_slice_alloc0(saddr.len);
	naddr.len = saddr.len;
	memcpy(naddr.addr, saddr.addr, saddr.len);
	return naddr;
}

gboolean li_ipv4_in_ipv4_net(guint32 target, guint32 match, guint32 networkmask) {
	return (target & networkmask) == (match & networkmask);
}

gboolean li_ipv6_in_ipv6_net(const unsigned char *target, const guint8 *match, guint network) {
	guint8 mask = network % 8;
	if (0 != memcmp(target, match, network / 8)) return FALSE;
	if (!mask) return TRUE;
	mask = ~(((guint) 1 << (8-mask)) - 1);
	return (target[network / 8] & mask) == (match[network / 8] & mask);
}

gboolean li_ipv6_in_ipv4_net(const unsigned char *target, guint32 match, guint32 networkmask) {
	static const guint8 ipv6match[] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF, 0, 0, 0, 0 };
	guint32 v4_target;
	if (!li_ipv6_in_ipv6_net(target, ipv6match, 96)) return  FALSE;
	memcpy(&v4_target, target + 12, 4);
	return li_ipv4_in_ipv4_net(v4_target, match, networkmask);
}

gboolean li_ipv4_in_ipv6_net(guint32 target, const guint8 *match, guint network) {
	guint8 ipv6[] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF, 0, 0, 0, 0 };
	memcpy(ipv6+12, &target, 4);
	return li_ipv6_in_ipv6_net(ipv6, match, network);
}

/* unused */
void li_gstring_replace_char_with_str_len(GString *gstr, gchar c, gchar *str, guint len) {
	guint i;
	for (i = 0; i < gstr->len; i++) {
		if (gstr->str[i] == c) {
			/* char found, replace */
			gstr->str[i] = str[0];
			if (len > 1)
				g_string_insert_len(gstr, i, &str[1], len-1);
			i += len - 1;
		}
	}
}

gboolean li_strncase_equal(const GString *str, const gchar *s, guint len) {
	if (str->len != len) return FALSE;
	return 0 == g_ascii_strncasecmp(str->str, s, len);
}

gboolean li_string_suffix(const GString *str, const gchar *s, gsize len) {
	if (str->len < len)
		return FALSE;

	return (strcmp(str->str + str->len - len, s) == 0);
}

gboolean li_string_prefix(const GString *str, const gchar *s, gsize len) {
	if (str->len < len)
		return FALSE;

	return (strncmp(str->str, s, len) == 0);
}

GString *li_string_assign_len(GString *string, const gchar *val, gssize len) {
	g_string_truncate(string, 0);
	g_string_append_len(string, val, len);
	return string;
}

void li_string_append_int(GString *dest, gint64 v) {
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
gsize li_dirent_buf_size(DIR * dirp) {
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

const char *li_remove_path(const char *path) {
	char *p = strrchr(path, G_DIR_SEPARATOR);
	if (NULL != p && *(p) != '\0') {
		return (p + 1);
	}
	return path;
}

GQuark li_sys_error_quark(void) {
	return g_quark_from_static_string("li-sys-error-quark");
}

gboolean _li_set_sys_error(GError **error, const gchar *msg, const gchar *file, int lineno) {
	int code = errno;
	g_set_error(error, LI_SYS_ERROR, code, "(%s:%d): %s: %s", file, lineno, msg, g_strerror(code));
	return FALSE;
}

void li_apr_sha1_base64(GString *dest, const GString *passwd) {
	GChecksum *sha1sum;
	gsize digestlen = g_checksum_type_get_length(G_CHECKSUM_SHA1);
	guint8 digest[digestlen+1];
	gchar *digest_base64;

	sha1sum = g_checksum_new(G_CHECKSUM_SHA1);
	g_checksum_update(sha1sum, GUSTR_LEN(passwd));
	g_checksum_get_digest(sha1sum, digest, &digestlen);
	g_checksum_free(sha1sum);

	digest_base64 = g_base64_encode(digest, digestlen);

	li_string_assign_len(dest, CONST_STR_LEN("{SHA}"));
	g_string_append(dest, digest_base64);

	g_free(digest_base64);
}

/*  The basic algorithm for this "apr-md5-crypt" comes from
 *  the FreeBSD 3.0 MD5 crypt() function, and was licensed as
 *  "BEER-WARE" from Poul-Henning Kamp.
 *
 *  This is a complete rewrite to use glib functions.
 *
 *  Note: security by obscurity is not real security - this
 *    still is "just" md5, don't trust it.
 */

#define APR1_MAGIC "$apr1$"

static void md5_crypt_to64(GString *dest, guint number, guint len) {
	static const gchar code[] = "./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
	for ( ; len-- > 0; ) {
		g_string_append_len(dest, code + (number & 63), 1);
		number /= 64;
	}
}

void li_apr_md5_crypt(GString *dest, const GString *password, const GString *salt) {
	guint i;
	GChecksum *md5sum;
	gsize digestlen = g_checksum_type_get_length(G_CHECKSUM_MD5);
	guint8 digest[digestlen];

	GString rsalt = { GSTR_LEN(salt), 0 };
	if (li_string_prefix(&rsalt, CONST_STR_LEN(APR1_MAGIC))) {
		rsalt.str += sizeof(APR1_MAGIC)-1;
		rsalt.len -= sizeof(APR1_MAGIC)-1;
	}
	if (rsalt.len > 8) rsalt.len = 8;
	for (i = 0; i < rsalt.len && rsalt.str[i] != '$'; i++) ;
	rsalt.len = i;

	md5sum = g_checksum_new(G_CHECKSUM_MD5);

	g_checksum_update(md5sum, GUSTR_LEN(password));
	g_checksum_update(md5sum, (guchar*) rsalt.str, rsalt.len);
	g_checksum_update(md5sum, GUSTR_LEN(password));
	g_checksum_get_digest(md5sum, digest, &digestlen);
	g_checksum_free(md5sum);

	md5sum = g_checksum_new(G_CHECKSUM_MD5);
	g_checksum_update(md5sum, GUSTR_LEN(password));
	g_checksum_update(md5sum, CONST_USTR_LEN(APR1_MAGIC));
	g_checksum_update(md5sum, (guchar*) rsalt.str, rsalt.len);

	for (i = password->len / 16; i-- > 0; ) {
		g_checksum_update(md5sum, digest, digestlen);
	}
	g_checksum_update(md5sum, digest, password->len % 16);

	for (i = password->len; i != 0; i /= 2) {
		if (i % 2) {
			g_checksum_update(md5sum, (guchar*) "", 1);
		} else {
			g_checksum_update(md5sum, (guchar*) password->str, 1);
		}
	}
	g_checksum_get_digest(md5sum, digest, &digestlen);
	g_checksum_free(md5sum);

	for (i = 0; i < 1000; i++) {
		md5sum = g_checksum_new(G_CHECKSUM_MD5);

		if (i % 2) {
			g_checksum_update(md5sum, GUSTR_LEN(password));
		} else {
			g_checksum_update(md5sum, digest, digestlen);
		}

		if (i % 3) {
			g_checksum_update(md5sum, (guchar*) rsalt.str, rsalt.len);
		}

		if (i % 7) {
			g_checksum_update(md5sum, GUSTR_LEN(password));
		}

		if (i % 2) {
			g_checksum_update(md5sum, digest, digestlen);
		} else {
			g_checksum_update(md5sum, GUSTR_LEN(password));
		}

		g_checksum_get_digest(md5sum, digest, &digestlen);

		g_checksum_free(md5sum);
	}

	li_g_string_clear(dest);
	g_string_append_len(dest, CONST_STR_LEN(APR1_MAGIC));
	g_string_append_len(dest, rsalt.str, rsalt.len);
	g_string_append_len(dest, CONST_STR_LEN("$"));
	md5_crypt_to64(dest, (digest[ 0] << 16) | (digest[ 6] << 8) | digest[12], 4);
	md5_crypt_to64(dest, (digest[ 1] << 16) | (digest[ 7] << 8) | digest[13], 4);
	md5_crypt_to64(dest, (digest[ 2] << 16) | (digest[ 8] << 8) | digest[14], 4);
	md5_crypt_to64(dest, (digest[ 3] << 16) | (digest[ 9] << 8) | digest[15], 4);
	md5_crypt_to64(dest, (digest[ 4] << 16) | (digest[10] << 8) | digest[ 5], 4);
	md5_crypt_to64(dest,                       digest[11]                   , 2);
}

void li_safe_crypt(GString *dest, const GString *password, const GString *salt) {
	if (g_str_has_prefix(salt->str, "$apr1$")) {
		li_apr_md5_crypt(dest, password, salt);
	} else {
#ifdef HAVE_CRYPT_R
		struct crypt_data buffer;

		memset(&buffer, 0, sizeof(buffer));

		g_string_assign(dest, crypt_r(password->str, salt->str, &buffer));
#else
		/* This is an acceptable hack: any library that uses crypt() itself is "broken"
		 * for threaded usage anyway; and our own usage is protected.
		 */
		static GStaticMutex crypt_mutex = G_STATIC_MUTEX_INIT;

		g_static_mutex_lock(&crypt_mutex);
		g_string_assign(dest, crypt(password->str, salt->str));
		g_static_mutex_unlock(&crypt_mutex);
#endif
	}
}


void li_g_queue_merge(GQueue *dest, GQueue *src) {
	assert(dest != src);
	if (g_queue_is_empty(src)) return; /* nothing to do */

	/* if dest is empty, just swap dest / src */
	if (g_queue_is_empty(dest)) {
		GQueue tmp = *src; *src = *dest; *dest = tmp;
	} else {
		/* link the two "lists", neither of them is empty */
		dest->tail->next = src->head;
		src->head->prev = dest->tail;
		/* update the queue tail and length */
		dest->tail = src->tail;
		dest->length += src->length;
		/* reset src */
		g_queue_init(src);
	}
}

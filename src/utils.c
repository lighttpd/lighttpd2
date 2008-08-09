
#include "utils.h"

#include <stdio.h>
#include <stdlib.h>

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

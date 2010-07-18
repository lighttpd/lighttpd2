
#include <lighttpd/memcached.h>

#include <lighttpd/utils.h>

/* IMPORTANT
 * In order to keep _release thread-safe the ev_io watcher keeps a
 * reference too while active; when the last reference is dropped
 * we don't have to stop the watcher, and everything else is
 * "thread-safe" if no one is using it anymore (refcount == 0)
 * (see memcached_{start,stop}_io)
 * This means we have to stop the watcher after all requests are done.
 *
 * Most "public" functions have to be called while they hold
 * a reference somehow; the other way this code gets executed is
 * through the ev_io callback, which is why we get an extra
 * reference there, so our refcount doesn't drop to 0 while
 * we are working.
 *
 * TODO: retry connect() once (per second?) if we have a request
 *   before we drop all requests
 */

GQuark li_memcached_error_quark() {
	return g_quark_from_static_string("memcached-error-quark");
}

#define BUFFER_CHUNK_SIZE 4*1024

typedef struct int_request int_request;
typedef enum {
	REQ_GET, REQ_SET
} req_type;

struct liMemcachedCon {
	struct ev_loop *loop;
	liSocketAddress addr;

	int refcount;

	ev_io con_watcher;
	int fd;
	ev_tstamp last_con_start;

	GQueue req_queue;
	int_request *cur_req;

	GQueue out;
	liBuffer *buf;

	GString *tmpstr;

	GError *err;

	/* read buffers */
	liBuffer *line, *data, *remaining;
	liMemcachedItem curitem;

	/* GET */
	gsize get_data_size;
	gboolean get_have_header;
};

struct int_request {
	liMemcachedRequest req;
	req_type type;

	GString *key;
	guint32 flags;
	ev_tstamp ttl;
	liBuffer *data;

	GList iter;
};

typedef struct {
	gsize pos, len;
	liBuffer *buf;
} send_item;

static void send_queue_push_buffer(GQueue *queue, liBuffer *buf, gsize start, gsize len) {
	send_item *i;
	if (!buf || !len) return;
	g_assert(start+len <= buf->used);

	li_buffer_acquire(buf);
	i = g_slice_new0(send_item);
	i->buf = buf;
	i->pos = start;
	i->len = len;

	g_queue_push_tail(queue, i);
}

static void send_queue_push_gstring(GQueue *queue, GString *str, liBuffer **pbuf) {
	liBuffer *buf = *pbuf;
	gsize pos;

	if (NULL != buf && (1 == buf->refcount)) {
		buf->used = 0;
	}
	if (NULL == buf || (buf->alloc_size - buf->used < str->len)) {
		li_buffer_release(buf);
		buf = li_buffer_new_slice(BUFFER_CHUNK_SIZE > str->len ? BUFFER_CHUNK_SIZE : str->len);
		*pbuf = buf;
	}

	pos = buf->used;
	memcpy(buf->addr + pos, str->str, str->len);
	buf->used += str->len;
	send_queue_push_buffer(queue, buf, pos, str->len);
}

static void send_queue_item_free(send_item *i) {
	if (!i) return;
	li_buffer_release(i->buf);
	g_slice_free(send_item, i);
}

static void send_queue_clean(GQueue *queue) {
	send_item *i;
	while (NULL != (i = g_queue_peek_head(queue))) {
		if (i->len != 0) return;
		li_buffer_release(i->buf);
		g_slice_free(send_item, i);
	}
}

static void send_queue_reset(GQueue *queue) {
	send_item *i;
	while (NULL != (i = g_queue_peek_head(queue))) {
		li_buffer_release(i->buf);
		g_slice_free(send_item, i);
	}
}

static void memcached_start_io(liMemcachedCon *con) {
	if (!((ev_watcher*) &con->con_watcher)->active) {
		li_memcached_con_acquire(con);
		li_ev_safe_unref_and_start(ev_io_start, con->loop, &con->con_watcher);
	}
}

static void memcached_stop_io(liMemcachedCon *con) {
	if (((ev_watcher*) &con->con_watcher)->active) {
		li_ev_safe_ref_and_stop(ev_io_stop, con->loop, &con->con_watcher);
		li_memcached_con_release(con);
	}
}

static void send_request(liMemcachedCon *con, int_request *req) {
	switch (req->type) {
	case REQ_GET:
		g_string_printf(con->tmpstr, "get %s\r\n", req->key->str);
		send_queue_push_gstring(&con->out, con->tmpstr, &con->buf);
		break;
	case REQ_SET:
		/* set <key> <flags> <exptime> <bytes>\r\n */

		g_string_printf(con->tmpstr, "set %s %"G_GUINT32_FORMAT" %"G_GUINT64_FORMAT" %"G_GSIZE_FORMAT"\r\n", req->key->str, req->flags, (guint64) req->ttl, req->data->used);
		send_queue_push_gstring(&con->out, con->tmpstr, &con->buf);
		send_queue_push_buffer(&con->out, req->data, 0, req->data->used);
		g_string_assign(con->tmpstr, "\r\n");
		send_queue_push_gstring(&con->out, con->tmpstr, &con->buf);
		break;
	}
}

static gboolean push_request(liMemcachedCon *con, int_request *req, GError **err) {
	UNUSED(err);

	li_memcached_con_acquire(con);

	send_request(con, req);

	req->iter.data = req;
	g_queue_push_tail_link(&con->req_queue, &req->iter);

	memcached_start_io(con);
	li_ev_io_set_events(con->loop, &con->con_watcher, EV_READ | EV_WRITE);

	return TRUE;
}

static void free_request(liMemcachedCon *con, int_request *req) {
	if (!req) return;

	li_memcached_con_release(con);

	if (NULL != req->iter.data) {
		req->iter.data = NULL;
		g_queue_unlink(&con->req_queue, &req->iter);
	}

	switch (req->type) {
	case REQ_GET:
		break;
	case REQ_SET:
		li_buffer_release(req->data);
		req->data = NULL;
		break;
	}

	g_string_free(req->key, TRUE);
	req->key = NULL;

	g_slice_free(int_request, req);
}

static void cancel_all_requests(liMemcachedCon *con) {
	int_request *req;
	GError *err1 = NULL, *err = NULL;
	if (con->err) {
		err1 = g_error_copy(con->err);
	} else {
		g_set_error(&err1, LI_MEMCACHED_ERROR, LI_MEMCACHED_CONNECTION, "Connection reset");
	}

	while (NULL != (req = g_queue_peek_head(&con->req_queue))) {
		if (NULL == err) {
			err = g_error_copy(err1);
		}

		if (req->req.callback) req->req.callback(&req->req, LI_MEMCACHED_RESULT_ERROR, NULL, &err);
	}

	if (NULL != err) g_clear_error(&err);
	if (NULL != err1) g_clear_error(&err1);
}

static void memcached_update_io(liMemcachedCon *con) {
	int events = 0;

	if (-1 == con->fd) return; /* not connected or in connect stage */

	if (0 < con->req_queue.length) events = events | EV_READ;
	if (0 < con->out.length) events = events | EV_WRITE;

	if (0 == events) {
		memcached_stop_io(con);
	} else {
		memcached_start_io(con);
		li_ev_io_set_events(con->loop, &con->con_watcher, events);
	}
}

static void memcached_connect(liMemcachedCon *con) {
	int s;
	struct sockaddr addr;
	socklen_t len;

	if (-1 != con->fd) return;

	s = con->con_watcher.fd;
	if (-1 == s) {
		/* reconnect limit */
		if (ev_now(con->loop) < con->last_con_start + 1) return;
		con->last_con_start = ev_now(con->loop);

		do {
			s = socket(con->addr.addr->plain.sa_family, SOCK_STREAM, 0);
		} while (-1 == s && errno == EINTR);
		if (-1 == s) {
			g_clear_error(&con->err);
			g_set_error(&con->err, LI_MEMCACHED_ERROR, LI_MEMCACHED_CONNECTION, "Couldn't open socket: %s", g_strerror(errno));
			return;
		}
		li_fd_init(s);
		ev_io_set(&con->con_watcher, s, 0);

		if (-1 == connect(s, &con->addr.addr->plain, con->addr.len)) {
			switch (errno) {
			case EINPROGRESS:
			case EALREADY:
			case EINTR:
				memcached_start_io(con);
				li_ev_io_add_events(con->loop, &con->con_watcher, EV_READ | EV_WRITE);
				break;
			default:
				g_clear_error(&con->err);
				g_set_error(&con->err, LI_MEMCACHED_ERROR, LI_MEMCACHED_CONNECTION, "Couldn't connect to '%s': %s",
					li_sockaddr_to_string(con->addr, con->tmpstr, TRUE)->str,
					g_strerror(errno));
				close(s);
				ev_io_set(&con->con_watcher, -1, 0);
				break;
			}
		} else {
			/* connect succeeded */
			con->fd = s;
			g_clear_error(&con->err);
			memcached_update_io(con);
		}

		return;
	}

	/* create new connection:
	 * see http://www.cyberconf.org/~cynbe/ref/nonblocking-connects.html
	 */

	/* Check to see if we can determine our peer's address. */
	len = sizeof(addr);
	if (getpeername(s, &addr, &len) == -1) {
		/* connect failed; find out why */
		int err;
		len = sizeof(err);
#ifdef SO_ERROR
		getsockopt(s, SOL_SOCKET, SO_ERROR, (void*)&err, &len);
#else
		{
			char ch;
			errno = 0;
			read(s, &ch, 1);
			err = errno;
		}
#endif
		g_clear_error(&con->err);
		g_set_error(&con->err, LI_MEMCACHED_ERROR, LI_MEMCACHED_CONNECTION, "Couldn't connect socket to '%s': %s",
			li_sockaddr_to_string(con->addr, con->tmpstr, TRUE)->str,
			g_strerror(err));

		close(s);
		memcached_stop_io(con);
		ev_io_set(&con->con_watcher, -1, 0);
	} else {
		/* connect succeeded */
		con->fd = s;
		g_clear_error(&con->err);
		memcached_update_io(con);
	}
}

static void reset_item(liMemcachedItem *item) {
	if (item->key) {
		g_string_free(item->key, TRUE);
		item->key = NULL;
	}
	item->flags = 0;
	item->ttl = 0;
	item->cas = 0;
	if (item->data) {
		li_buffer_release(item->data);
		item->data = NULL;
	}
}

static void close_con(liMemcachedCon *con) {
	if (con->line) con->line->used = 0;
	if (con->remaining) con->remaining->used = 0;
	if (con->data) con->data->used = 0;
	if (con->buf) con->buf->used = 0;
	reset_item(&con->curitem);
	send_queue_reset(&con->out);

	memcached_stop_io(con);
	close(con->con_watcher.fd);
	con->fd = -1;
	ev_io_set(&con->con_watcher, -1, 0);
	con->cur_req = NULL;
	cancel_all_requests(con);
	memcached_connect(con);
}

static void add_remaining(liMemcachedCon *con, gchar *addr, gsize len) {
	liBuffer *rem = con->remaining;
	if (!rem) rem = con->remaining = li_buffer_new_slice(MAX(BUFFER_CHUNK_SIZE, len));
	if (rem->used + len > rem->alloc_size) {
		rem = li_buffer_new_slice(MAX(BUFFER_CHUNK_SIZE, rem->used + len));
		memcpy(rem->addr, con->remaining->addr, (rem->used = con->remaining->used));
		li_buffer_release(con->remaining);
		con->remaining = rem;
	}

	memcpy(rem->addr + rem->used, addr, len);
	rem->used += len;
}

/** repeats read after EINTR */
static ssize_t net_read(int fd, void *buf, ssize_t nbyte) {
	ssize_t r;
	while (-1 == (r = read(fd, buf, nbyte))) {
		switch (errno) {
		case EINTR:
			/* Try again */
			break;
		default:
			/* report error */
			return r;
		}
	}
	/* return bytes read */
	return r;
}

static gboolean try_read_line(liMemcachedCon *con) {
	liBuffer *line;
	ssize_t r;

	if (!con->line) con->line = li_buffer_new_slice(BUFFER_CHUNK_SIZE);
	if (!con->remaining) con->remaining = li_buffer_new_slice(BUFFER_CHUNK_SIZE);

	/* if we have remaining data use it for a new line */
	if (con->line->used == 0 && con->remaining->used > 0) {
		liBuffer *tmp = con->remaining; con->remaining = con->line; con->line = tmp;
	}

	g_assert(NULL == con->remaining || 0 == con->remaining->used); /* there shouldn't be any data in remaining while we fill con->line */

	line = con->line;

	if (line->used > 0) {
		/* search for \r\n */
		gchar *addr = line->addr;
		gsize i, len = line->used;
		for (i = 0; i < len; i++) {
			if (addr[i] == '\r') {
				i++;
				if (i < len && addr[i] == '\n') {
					add_remaining(con, addr + i+1, len - (i+1));
					line->used = i-1;
					line->addr[i-1] = '\0';
					return TRUE;
				}
			}
		}
	}

	if (line->used > 1024) {
		/* Protocol error: we don't parse line longer than 1024 */
		g_clear_error(&con->err);
		g_set_error(&con->err, LI_MEMCACHED_ERROR, LI_MEMCACHED_CONNECTION, "Protocol error: line too long");
		close_con(con);
		return FALSE;
	}

	/* need more data */
	r = net_read(con->fd, line->addr + line->used, line->alloc_size - line->used);
	if (r == 0) {
		/* EOF */
		g_clear_error(&con->err);
		g_set_error(&con->err, LI_MEMCACHED_ERROR, LI_MEMCACHED_CONNECTION, "Connection closed by peer");
		close_con(con);
		return FALSE;
	} else if (r < 0) {
		switch (errno) {
		case EAGAIN:
#if EWOULDBLOCK != EAGAIN
		case EWOULDBLOCK:
#endif
			break;
		default:
			g_clear_error(&con->err);
			g_set_error(&con->err, LI_MEMCACHED_ERROR, LI_MEMCACHED_CONNECTION, "Connection closed: %s", g_strerror(errno));
			close_con(con);
			break;
		}
		return FALSE;
	}

	line->used += r;

	if (line->used > 0) {
		/* search for \r\n */
		gchar *addr = line->addr;
		gsize i, len = line->used;
		for (i = 0; i < len; i++) {
			if (addr[i] == '\r') {
				i++;
				if (i < len && addr[i] == '\n') {
					add_remaining(con, addr + i+1, len - (i+1));
					line->used = i-1;
					line->addr[i-1] = '\0';
					return TRUE;
				}
			}
		}
	}

	return FALSE;
}

static gboolean try_read_data(liMemcachedCon *con, gsize datalen) {
	liBuffer *data;
	ssize_t r;

	datalen += 2; /* \r\n */

	/* if we have remaining data use it for a new line */
	if ((!con->data || con->data->used == 0) && con->remaining && con->remaining->used > 0) {
		liBuffer *tmp = con->remaining; con->remaining = con->data; con->data = tmp;
	}

	if (!con->data) con->data = li_buffer_new_slice(MAX(BUFFER_CHUNK_SIZE, datalen));

	if (con->data->alloc_size < datalen) {
		data = li_buffer_new_slice(MAX(BUFFER_CHUNK_SIZE, datalen));
		memcpy(data->addr, con->data->addr, (data->used = con->data->used));
		li_buffer_release(con->data);
		con->data = data;
	}

	g_assert(NULL == con->remaining || 0 == con->remaining->used); /* there shouldn't be any data in remaining while we fill con->data */

	data = con->data;

	if (data->used < datalen) {
		/* read more data */
		r = net_read(con->fd, data->addr + data->used, data->alloc_size - data->used);
		if (r == 0) {
			/* EOF */
			g_clear_error(&con->err);
			g_set_error(&con->err, LI_MEMCACHED_ERROR, LI_MEMCACHED_CONNECTION, "Connection closed by peer");
			close_con(con);
			return FALSE;
		} else if (r < 0) {
			switch (errno) {
			case EAGAIN:
#if EWOULDBLOCK != EAGAIN
			case EWOULDBLOCK:
#endif
				break;
			default:
				g_clear_error(&con->err);
				g_set_error(&con->err, LI_MEMCACHED_ERROR, LI_MEMCACHED_CONNECTION, "Connection closed: %s", g_strerror(errno));
				close_con(con);
				break;
			}
			return FALSE;
		}

		data->used += r;
	}

	if (data->used >= datalen) {
		if (data->addr[datalen-2] != '\r' || data->addr[datalen-1] != '\n') {
			/* Protocol error: data block not terminated with \r\n */
			g_clear_error(&con->err);
			g_set_error(&con->err, LI_MEMCACHED_ERROR, LI_MEMCACHED_CONNECTION, "Protocol error: data block not terminated with \\r\\n");
			close_con(con);
			return FALSE;
		}
		add_remaining(con, data->addr + datalen, data->used - datalen);
		data->used = datalen - 2;
		data->addr[datalen-2] = '\0';
		return TRUE;
	}

	return FALSE;
}


static void handle_read(liMemcachedCon *con) {
	int_request *cur;

	if (NULL == (cur = con->cur_req)) {
		cur = con->cur_req = g_queue_peek_head(&con->req_queue);

		if (NULL == cur) {
			/* unexpected read event, perhaps just eof */
			g_clear_error(&con->err);
			g_set_error(&con->err, LI_MEMCACHED_ERROR, LI_MEMCACHED_CONNECTION, "Connection closed: unexpected read event");
			close_con(con);
			return;
		}

		reset_item(&con->curitem);
		if (con->data) con->data->used = 0;
		if (con->line) con->line->used = 0;

		/* init read state */
		switch (cur->type) {
		case REQ_GET:
			con->get_data_size = 0;
			con->get_have_header = FALSE;
			break;
		case REQ_SET:
			break;
		}
	}

	switch (cur->type) {
	case REQ_GET:
		if (!con->get_have_header) {
			char *pos, *next;

			/* wait for header line */
			if (!try_read_line(con)) return;

			con->get_have_header = TRUE;

			if (3 == con->line->used && 0 == memcmp("END", con->line->addr, 3)) {
				/* key not found */
				if (cur->req.callback) {
					cur->req.callback(&cur->req, LI_MEMCACHED_NOT_FOUND, NULL, NULL);
				}
				con->cur_req = NULL;
				free_request(con, cur);
				return;
			}

			/* con->line is 0 terminated */

			if (0 != strncmp("VALUE ", con->line->addr, 6)) {
				g_clear_error(&con->err);
				g_set_error(&con->err, LI_MEMCACHED_ERROR, LI_MEMCACHED_CONNECTION, "Protocol error: Unexpected response for GET: '%s'", con->line->addr);
				close_con(con);
				return;
			}

			/* VALUE <key> <flags> <bytes> [<cas unique>]\r\n */

			/* <key> */
			pos = con->line->addr + 6;
			next = strchr(pos, ' ');
			if (NULL == next) goto req_get_header_error;

			con->curitem.key = g_string_new_len(pos, next - pos);

			/* <flags> */
			pos = next + 1;
			con->curitem.flags = strtoul(pos, &next, 10);
			if (' ' != *next || pos == next) goto req_get_header_error;

			/* <bytes> */
			pos = next + 1;
			con->get_data_size = g_ascii_strtoll(pos, &next, 10);
			if (pos == next) goto req_get_header_error;

			/* [<cas unique>] */
			if (' ' == *next) {
				pos = next + 1;
				con->curitem.cas = g_ascii_strtoll(pos, &next, 10);
				if (pos == next) goto req_get_header_error;
			}

			if ('\0' != *next) {
				goto req_get_header_error;
			}

			con->line->used = 0;

			goto req_get_header_done;

req_get_header_error:
			g_clear_error(&con->err);
			g_set_error(&con->err, LI_MEMCACHED_ERROR, LI_MEMCACHED_CONNECTION, "Protocol error: Couldn't parse VALUE respone: '%s'", con->line->addr);
			close_con(con);
			return;

req_get_header_done: ;
		}
		if (NULL == con->data || con->data->used < con->get_data_size) {
			/* wait for data */
			if (!try_read_data(con, con->get_data_size)) return;
		}
		/* wait for END\r\n */
		if (!try_read_line(con)) return;

		if (3 == con->line->used && 0 == memcmp("END", con->line->addr, 3)) {
			/* Move data to item */
			con->curitem.data = con->data;
			con->data = NULL;
			if (cur->req.callback) {
				cur->req.callback(&cur->req, LI_MEMCACHED_OK, &con->curitem, NULL);
			}
			reset_item(&con->curitem);
		} else {
			g_clear_error(&con->err);
			g_set_error(&con->err, LI_MEMCACHED_ERROR, LI_MEMCACHED_CONNECTION, "Protocol error: GET response not terminated with END (got '%s')", con->line->addr);
			close_con(con);
			return;
		}

		con->cur_req = NULL;
		free_request(con, cur);
		return;

	case REQ_SET:
		if (!try_read_line(con)) return;

		if (6 == con->line->used && 0 == memcmp("STORED", con->line->addr, 6)) {
			if (cur->req.callback) {
				cur->req.callback(&cur->req, LI_MEMCACHED_OK, NULL, NULL);
			}
		} else {
			g_clear_error(&con->err);
			g_set_error(&con->err, LI_MEMCACHED_ERROR, LI_MEMCACHED_CONNECTION, "Protocol error: unepxected SET response: '%s'", con->line->addr);
			close_con(con);
			return;
		}

		con->cur_req = NULL;
		free_request(con, cur);
		return;
	}
}

static void memcached_io_cb(struct ev_loop *loop, ev_io *w, int revents) {
	liMemcachedCon *con = (liMemcachedCon*) w->data;
	UNUSED(loop);

	if (1 == g_atomic_int_get(&con->refcount) && w->active) {
		memcached_stop_io(con);
		return;
	}

	if (-1 == con->fd) {
		memcached_connect(con);
		return;
	}

	li_memcached_con_acquire(con); /* make sure con isn't freed in the middle of something */

	if (revents | EV_WRITE) {
		int i;
		ssize_t written, len;
		gchar *data;
		send_item *si;

		si = g_queue_peek_head(&con->out);

		for (i = 0; si && (i < 10); i++) { /* don't send more than 10 chunks */
			data = si->buf->addr + si->pos;
			len = si->len;
			written = write(w->fd, data, len);
			if (written < 0) {
				switch (errno) {
				case EINTR:
					continue;
				case EAGAIN:
#if EWOULDBLOCK != EAGAIN
				case EWOULDBLOCK:
#endif
					goto write_eagain;
				default: /* Fatal error, connection has to be closed */
					g_clear_error(&con->err);
					g_set_error(&con->err, LI_MEMCACHED_ERROR, LI_MEMCACHED_CONNECTION, "Couldn't write socket '%s': %s",
						li_sockaddr_to_string(con->addr, con->tmpstr, TRUE)->str,
						g_strerror(errno));
					close_con(con);
					goto out;
				}
			} else {
				si->pos += written;
				si->len -= written;
				if (0 == si->len) {
					send_queue_item_free(si);
					g_queue_pop_head(&con->out);
					si = g_queue_peek_head(&con->out);
				}
			}
		}

write_eagain:
		send_queue_clean(&con->out);
	}

	if (revents | EV_READ) {
		do {
			handle_read(con);
		} while (con->remaining && con->remaining->used > 0);
	}

out:
	memcached_update_io(con);
	li_memcached_con_release(con);
}


liMemcachedCon* li_memcached_con_new(struct ev_loop *loop, liSocketAddress addr) {
	liMemcachedCon* con = g_slice_new0(liMemcachedCon);

	con->refcount = 1;
	con->loop = loop;
	con->addr = li_sockaddr_dup(addr);
	con->tmpstr = g_string_sized_new(511);

	con->fd = -1;
	ev_io_init(&con->con_watcher, memcached_io_cb, -1, 0);
	con->con_watcher.data = con;

	memcached_connect(con);

	return con;
}

static void li_memcached_con_free(liMemcachedCon* con) {
	if (!con) return;

	if (-1 != con->con_watcher.fd) {
		close(con->con_watcher.fd);
		/* as io has a reference on con, we don't need to stop it here */
		ev_io_set(&con->con_watcher, -1, 0);
		con->fd = -1;
	}

	send_queue_reset(&con->out);
	cancel_all_requests(con);

	li_buffer_release(con->buf);
	li_buffer_release(con->line);
	li_buffer_release(con->remaining);
	li_buffer_release(con->data);
	reset_item(&con->curitem);

	li_sockaddr_clear(&con->addr);
	g_string_free(con->tmpstr, TRUE);

	g_clear_error(&con->err);

	g_slice_free(liMemcachedCon, con);
}

void li_memcached_con_release(liMemcachedCon* con) {
	if (!con) return;
	assert(g_atomic_int_get(&con->refcount) > 0);
	if (g_atomic_int_dec_and_test(&con->refcount)) {
		li_memcached_con_free(con);
	}
}

void li_memcached_con_acquire(liMemcachedCon* con) {
	assert(g_atomic_int_get(&con->refcount) > 0);
	g_atomic_int_inc(&con->refcount);
}


liMemcachedRequest* li_memcached_get(liMemcachedCon *con, GString *key, liMemcachedCB callback, gpointer cb_data, GError **err) {
	int_request* req;

	if (!li_memcached_is_key_valid(key)) {
		g_set_error(err, LI_MEMCACHED_ERROR, LI_MEMCACHED_BAD_KEY, "Invalid key: '%s'", key->str);
		return NULL;
	}

	if (-1 == con->fd) memcached_connect(con);
	if (-1 == con->fd) {
		if (NULL == con->err) {
			g_set_error(err, LI_MEMCACHED_ERROR, LI_MEMCACHED_CONNECTION, "Not connected");
		} else if (err) {
			*err = g_error_copy(con->err);
		}
		return NULL;
	}

	req = g_slice_new0(int_request);
	req->req.callback = callback;
	req->req.cb_data = cb_data;

	req->type = REQ_GET;
	req->key = g_string_new_len(GSTR_LEN(key));

	if (!push_request(con, req, err)) {
		free_request(con, req);
		return NULL;
	}

	return &req->req;
}

liMemcachedRequest* li_memcached_set(liMemcachedCon *con, GString *key, guint32 flags, ev_tstamp ttl, liBuffer *data, liMemcachedCB callback, gpointer cb_data, GError **err) {
	int_request* req;

	if (!li_memcached_is_key_valid(key)) {
		g_set_error(err, LI_MEMCACHED_ERROR, LI_MEMCACHED_BAD_KEY, "Invalid key: '%s'", key->str);
		return NULL;
	}

	if (-1 == con->fd) memcached_connect(con);
	if (-1 == con->fd) {
		if (NULL == con->err) {
			g_set_error(err, LI_MEMCACHED_ERROR, LI_MEMCACHED_CONNECTION, "Not connected");
		} else if (err) {
			*err = g_error_copy(con->err);
		}
		return NULL;
	}

	req = g_slice_new0(int_request);
	req->req.callback = callback;
	req->req.cb_data = cb_data;

	req->type = REQ_SET;
	req->key = g_string_new_len(GSTR_LEN(key));
	req->flags = flags;
	req->ttl = ttl;
	li_buffer_acquire(data);
	req->data = data;

	if (!push_request(con, req, err)) {
		free_request(con, req);
		return NULL;
	}

	return &req->req;
}

/* if length(key) <= 250 and all chars x: 0x20 < x < 0x7f the key
 * remains untouched; otherwise it gets replaced with its sha1hex hash
 * so in most cases the key stays readable, and we have a good fallback
 */

void li_memcached_mutate_key(GString *key) {
	GChecksum *hash;

	if (li_memcached_is_key_valid(key)) return;

	hash = g_checksum_new(G_CHECKSUM_SHA1);

	g_checksum_update(hash, (const guchar *) GSTR_LEN(key));
	g_string_assign(key, g_checksum_get_string(hash));

	g_checksum_free(hash);
}

gboolean li_memcached_is_key_valid(GString *key) {
	guint i;

	if (key->len > 250 || 0 == key->len) return FALSE;

	for (i = 0; i < key->len; i++) {
		if (key->str[i] <= 0x20 || key->str[i] >= 0x7f) return FALSE;
	}

	return TRUE;
}

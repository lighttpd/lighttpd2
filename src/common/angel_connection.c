
#include <lighttpd/utils.h>
#include <lighttpd/angel_data.h>
#include <lighttpd/angel_connection.h>

#define ANGEL_MAGIC ((gint32) 0x8a930a9f)

static void close_fd_array(GArray *fds);

typedef enum {
	ANGEL_CALL_SEND_SIMPLE = 1,
	ANGEL_CALL_SEND_CALL = 2,
	ANGEL_CALL_SEND_RESULT = 3
} angel_call_send_t;

typedef struct {
	enum { ANGEL_CONNECTION_ITEM_GSTRING, ANGEL_CONNECTION_ITEM_FDS } type;
	union {
		struct {
			GString *buf;
			guint pos;
		} string;
		struct {
			GArray *fds;
			guint pos;
		} fds;
	} value;
} angel_connection_send_item_t;

static void send_queue_push_string(GQueue *queue, GString *buf) {
	angel_connection_send_item_t *i;
	if (!buf || !buf->len) return;

	i = g_slice_new0(angel_connection_send_item_t);
	i->type = ANGEL_CONNECTION_ITEM_GSTRING;
	i->value.string.buf = buf;
	i->value.string.pos = 0;

	g_queue_push_tail(queue, i);
}

static void send_queue_push_fds(GQueue *queue, GArray *fds) {
	angel_connection_send_item_t *i;
	if (!fds || !fds->len) return;

	i = g_slice_new0(angel_connection_send_item_t);
	i->type = ANGEL_CONNECTION_ITEM_FDS;
	i->value.fds.fds = fds;
	i->value.fds.pos = 0;

	g_queue_push_tail(queue, i);
}

static void send_queue_item_free(angel_connection_send_item_t *i) {
	if (!i) return;
	switch (i->type) {
	case ANGEL_CONNECTION_ITEM_GSTRING:
		g_string_free(i->value.string.buf, TRUE);
		break;
	case ANGEL_CONNECTION_ITEM_FDS:
		close_fd_array(i->value.fds.fds);
		g_array_free(i->value.fds.fds, TRUE);
		break;
	}
	g_slice_free(angel_connection_send_item_t, i);
}

static void send_queue_clean(GQueue *queue) {
	angel_connection_send_item_t *i;
	while (NULL != (i = g_queue_peek_head(queue))) {
		switch (i->type) {
		case ANGEL_CONNECTION_ITEM_GSTRING:
			if (i->value.string.pos < i->value.string.buf->len) return;
			g_string_free(i->value.string.buf, TRUE);
			break;
		case ANGEL_CONNECTION_ITEM_FDS:
			if (i->value.fds.pos < i->value.fds.fds->len) return;
			close_fd_array(i->value.fds.fds);
			g_array_free(i->value.fds.fds, TRUE);
			break;
		}
		g_queue_pop_head(queue);
		g_slice_free(angel_connection_send_item_t, i);
	}
}

GQuark li_angel_call_error_quark(void) {
	return g_quark_from_static_string("angel-call-error-quark");
}
GQuark li_angel_connection_error_quark(void) {
	return g_quark_from_static_string("angel-connection-error-quark");
}

static gboolean angel_fill_buffer(liAngelConnection *acon, guint need, GError **err) {
	gsize old_len;
	ssize_t want, r;
	if (acon->in.pos > 0) {
		g_string_erase(acon->in.data, 0, acon->in.pos);
		acon->in.pos = 0;
	}
	if (acon->in.data->len >= need) return TRUE;
	
	want = need - acon->in.data->len; /* always > 0 */
	old_len = acon->in.data->len;
	g_string_set_size(acon->in.data, need);
	for ( ; want > 0; ) {
		r = read(acon->fd, acon->in.data->str + old_len, want);
		if (r < 0) {
			switch (errno) {
			case EINTR:
				continue;
			case EAGAIN:
#if EWOULDBLOCK != EAGAIN
			case EWOULDBLOCK:
#endif
				g_string_set_size(acon->in.data, old_len);
				return TRUE;
			case ECONNRESET:
				g_set_error(err, LI_ANGEL_CONNECTION_ERROR, LI_ANGEL_CONNECTION_RESET, "eof");
				g_string_set_size(acon->in.data, old_len);
				return FALSE;
			default:
				g_set_error(err, LI_ANGEL_CONNECTION_ERROR, LI_ANGEL_CONNECTION_CLOSED,
					"read error: %s", g_strerror(errno));
				g_string_set_size(acon->in.data, old_len);
				return FALSE;
			}
		} else if (r == 0) { /* eof */
			g_set_error(err, LI_ANGEL_CONNECTION_ERROR, LI_ANGEL_CONNECTION_RESET, "eof");
			g_string_set_size(acon->in.data, old_len);
			return FALSE;
		} else {
			want -= r;
			old_len += r;
		}
	}

	g_string_set_size(acon->in.data, old_len);
	return TRUE;
}

static void close_fd_array(GArray *fds) {
	guint i;
	for (i = 0; i < fds->len; i++) {
		close(g_array_index(fds, int, i));
	}
	g_array_set_size(fds, 0);
}

static gboolean angel_dispatch(liAngelConnection *acon, GError **err) {
	gint32 id = acon->parse.id, type = acon->parse.type;
	liAngelCall *call;

	switch (type) {
	case ANGEL_CALL_SEND_SIMPLE:
		if (-1 != id) {
			g_set_error(err, LI_ANGEL_CONNECTION_ERROR, LI_ANGEL_CONNECTION_INVALID_DATA,
				"Invalid id: %i, should be -1 for simple call", (gint) id);
			close_fd_array(acon->parse.fds);
			return FALSE;
		}

		if (acon->parse.error->len > 0 || acon->parse.fds->len > 0) {
			g_set_error(err, LI_ANGEL_CONNECTION_ERROR, LI_ANGEL_CONNECTION_INVALID_DATA,
				"Wrong data in call");
			close_fd_array(acon->parse.fds);
			return FALSE;
		}
		acon->recv_call(acon, GSTR_LEN(acon->parse.mod), GSTR_LEN(acon->parse.action),
			id, acon->parse.data);
		break;
	case ANGEL_CALL_SEND_CALL:
		if (-1 == id) {
			g_set_error(err, LI_ANGEL_CONNECTION_ERROR, LI_ANGEL_CONNECTION_INVALID_DATA,
				"Invalid id: -1, should be >= 0 for call");
			close_fd_array(acon->parse.fds);
			return FALSE;
		}

		if (acon->parse.error->len > 0 || acon->parse.fds->len > 0) {
			g_set_error(err, LI_ANGEL_CONNECTION_ERROR, LI_ANGEL_CONNECTION_INVALID_DATA,
				"Wrong data in call");
			close_fd_array(acon->parse.fds);
			return FALSE;
		}
		acon->recv_call(acon, GSTR_LEN(acon->parse.mod), GSTR_LEN(acon->parse.action),
			id, acon->parse.data);
		break;
	case ANGEL_CALL_SEND_RESULT:
		g_mutex_lock(acon->mutex);
			if (!li_idlist_is_used(acon->call_id_list, id)) {
				g_mutex_unlock(acon->mutex);
				g_set_error(err, LI_ANGEL_CONNECTION_ERROR, LI_ANGEL_CONNECTION_INVALID_DATA,
					"Invalid id: %i", (gint) id);
				close_fd_array(acon->parse.fds);
				return FALSE;
			}
			li_idlist_put(acon->call_id_list, id);
			call = NULL;
			if ((guint) id < acon->call_table->len) {
				call = (liAngelCall*) g_ptr_array_index(acon->call_table, id);
				g_ptr_array_index(acon->call_table, id) = NULL;
			}
			if (NULL != call) {
				call->id = -1;

				call->result.error = acon->parse.error;
				call->result.data = acon->parse.data;
				call->result.fds = acon->parse.fds;
				acon->parse.error = g_string_sized_new(0);
				acon->parse.data = g_string_sized_new(0);
				acon->parse.fds = g_array_new(FALSE, FALSE, sizeof(int));
				li_event_async_send(&call->result_watcher);
			} else {
				/* timeout - call is gone */
				g_string_truncate(acon->parse.error, 0);
				g_string_truncate(acon->parse.data, 0);
				close_fd_array(acon->parse.fds);
			}
		g_mutex_unlock(acon->mutex);

		break;
	default:
		g_set_error(err, LI_ANGEL_CONNECTION_ERROR, LI_ANGEL_CONNECTION_INVALID_DATA,
			"Invalid type: %i", (gint) type);
		close_fd_array(acon->parse.fds);
		return FALSE;
	}

	return TRUE;
}

static gboolean angel_connection_read(liAngelConnection *acon, GError **err) {
	for ( ;; ) {
		if (!acon->parse.have_header) {
			gint32 magic;
			if (!angel_fill_buffer(acon, 8*4, err)) return FALSE;
			if (acon->in.data->len - acon->in.pos < 8*4) return TRUE; /* need more data */

			if (!li_angel_data_read_int32(&acon->in, &magic, err)) return FALSE;
			if (!li_angel_data_read_int32(&acon->in, &acon->parse.type, err)) return FALSE;
			if (!li_angel_data_read_int32(&acon->in, &acon->parse.id, err)) return FALSE;
			if (!li_angel_data_read_int32(&acon->in, &acon->parse.mod_len, err)) return FALSE;
			if (!li_angel_data_read_int32(&acon->in, &acon->parse.action_len, err)) return FALSE;
			if (!li_angel_data_read_int32(&acon->in, &acon->parse.error_len, err)) return FALSE;
			if (!li_angel_data_read_int32(&acon->in, &acon->parse.data_len, err)) return FALSE;
			if (!li_angel_data_read_int32(&acon->in, &acon->parse.missing_fds, err)) return FALSE;

			if (ANGEL_MAGIC != magic) {
				g_set_error(err, LI_ANGEL_CONNECTION_ERROR, LI_ANGEL_CONNECTION_INVALID_DATA,
					"Invalid magic: 0x%x (should be 0x%x)", (gint) magic, (gint) ANGEL_MAGIC);
				return FALSE;
			}

			acon->parse.body_size = acon->parse.mod_len + acon->parse.action_len + acon->parse.error_len + acon->parse.data_len;
			acon->parse.have_header = TRUE;
		}

		if (!angel_fill_buffer(acon, acon->parse.body_size, err)) return FALSE;
		if (acon->in.data->len - acon->in.pos < acon->parse.body_size) return TRUE; /* need more data */
		while (acon->parse.missing_fds > 0) {
			int fd = -1;
			switch (li_receive_fd(acon->fd, &fd)) {
			case 0:
				g_array_append_val(acon->parse.fds, fd);
				acon->parse.missing_fds--;
				break;
			case -1:
				g_set_error(err, LI_ANGEL_CONNECTION_ERROR, LI_ANGEL_CONNECTION_CLOSED,
					"receive fd error: %s", g_strerror(errno));
				return FALSE;
			case -2:
				return TRUE; /* need more data */
			}
		}

		acon->parse.have_header = FALSE;
		if (!li_angel_data_read_mem(&acon->in, &acon->parse.mod, acon->parse.mod_len, err)) return FALSE;
		if (!li_angel_data_read_mem(&acon->in, &acon->parse.action, acon->parse.action_len, err)) return FALSE;
		if (!li_angel_data_read_mem(&acon->in, &acon->parse.error, acon->parse.error_len, err)) return FALSE;
		if (!li_angel_data_read_mem(&acon->in, &acon->parse.data, acon->parse.data_len, err)) return FALSE;

		if (!angel_dispatch(acon, err)) return FALSE;

		g_string_truncate(acon->parse.error, 0);
		g_string_truncate(acon->parse.data, 0);
		g_array_set_size(acon->parse.fds, 0);
	}
}

static void angel_connection_io_cb(liEventBase *watcher, int events) {
	liAngelConnection *acon = LI_CONTAINER_OF(li_event_io_from(watcher), liAngelConnection, fd_watcher);

	if (events & LI_EV_WRITE) {
		GString *out_str;
		int i;
		ssize_t written, len;
		gchar *data;
		gboolean out_queue_empty;
		angel_connection_send_item_t *send_item;
		int fd = li_event_io_fd(&acon->fd_watcher);

		g_mutex_lock(acon->mutex);
			send_item = g_queue_peek_head(acon->out);
		g_mutex_unlock(acon->mutex);

		for (i = 0; send_item && (i < 10); i++) { /* don't send more than 10 chunks */
			switch (send_item->type) {
			case ANGEL_CONNECTION_ITEM_GSTRING:
				out_str = send_item->value.string.buf;
				written = send_item->value.string.pos;
				data = out_str->str + written;
				len = out_str->len - written;
				while (len > 0) {
					written = write(fd, data, len);
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
							li_event_clear(&acon->out_notify_watcher);
							li_event_clear(&acon->fd_watcher);
							acon->close_cb(acon, NULL); /* TODO: set err */
							return;
						}
					} else {
						data += written;
						len -= written;
						send_item->value.string.pos += written;
					}
				}
				break;

			case ANGEL_CONNECTION_ITEM_FDS:
				while (send_item->value.fds.pos < send_item->value.fds.fds->len) {
					switch (li_send_fd(fd, g_array_index(send_item->value.fds.fds, int, send_item->value.fds.pos))) {
					case  0:
						send_item->value.fds.pos++;
						continue;
					case -1: /* Fatal error, connection has to be closed */
						li_event_clear(&acon->out_notify_watcher);
						li_event_clear(&acon->fd_watcher);
						acon->close_cb(acon, NULL); /* TODO: set err */
						return;
					case -2: goto write_eagain;
					}
				}
				break;
			}

			g_mutex_lock(acon->mutex);
				send_queue_clean(acon->out);
				send_item = g_queue_peek_head(acon->out);
			g_mutex_unlock(acon->mutex);
		}

write_eagain:
		g_mutex_lock(acon->mutex);
		send_queue_clean(acon->out);
		out_queue_empty = (0 == acon->out->length);
		g_mutex_unlock(acon->mutex);

		if (out_queue_empty) li_event_io_rem_events(&acon->fd_watcher, LI_EV_WRITE);
	}

	if (events & LI_EV_READ) {
		GError *err = NULL;
		if (!angel_connection_read(acon, &err)) {
			li_event_clear(&acon->out_notify_watcher);
			li_event_clear(&acon->fd_watcher);
			acon->close_cb(acon, err);
		}
	}
}

static void angel_connection_out_notify_cb(liEventBase *watcher, int events) {
	liAngelConnection *acon = LI_CONTAINER_OF(li_event_async_from(watcher), liAngelConnection, out_notify_watcher);
	UNUSED(events);
	li_event_io_add_events(&acon->fd_watcher, LI_EV_WRITE);
}

/* create connection */
liAngelConnection* li_angel_connection_new(liEventLoop *loop, int fd, gpointer data,
                                          liAngelReceiveCallCB recv_call, liAngelCloseCB close_cb) {
	liAngelConnection *acon = g_slice_new0(liAngelConnection);

	acon->data = data;
	acon->mutex = g_mutex_new();
	acon->fd = fd;
	acon->call_id_list = li_idlist_new(65535);
	acon->call_table = g_ptr_array_new();

	li_event_io_init(loop, &acon->fd_watcher, angel_connection_io_cb, fd, LI_EV_READ);
	li_event_set_keep_loop_alive(&acon->fd_watcher, FALSE);
	li_event_start(&acon->fd_watcher);

	li_event_async_init(loop, &acon->out_notify_watcher, angel_connection_out_notify_cb);

	acon->out = g_queue_new();
	acon->in.data = g_string_sized_new(1024);
	acon->in.pos = 0;

	acon->parse.mod = g_string_sized_new(0);
	acon->parse.action = g_string_sized_new(0);
	acon->parse.error = g_string_sized_new(0);
	acon->parse.data = g_string_sized_new(0);
	acon->parse.fds = g_array_new(FALSE, FALSE, sizeof(int));

	acon->recv_call = recv_call;
	acon->close_cb = close_cb;

	return acon;
}

void li_angel_connection_free(liAngelConnection *acon) {
	angel_connection_send_item_t *send_item;
	guint i;

	if (!acon) return;

	close(acon->fd);
	acon->fd = -1;

	for (i = 0; i < acon->call_table->len; i++) {
		liAngelCall *acall = g_ptr_array_index(acon->call_table, i);
		if (NULL == acall) continue;
		g_ptr_array_index(acon->call_table, i) = NULL;

		if (NULL != acall->callback) {
			acall->callback(acall->context, TRUE, NULL, NULL, NULL);
		}
		li_angel_call_free(acall);
	}
	g_ptr_array_free(acon->call_table, TRUE);

	g_mutex_free(acon->mutex);
	acon->mutex = NULL;

	li_event_clear(&acon->out_notify_watcher);
	li_event_clear(&acon->fd_watcher);

	li_idlist_free(acon->call_id_list);
	while (NULL != (send_item = g_queue_pop_head(acon->out))) {
		send_queue_item_free(send_item);
	}
	g_queue_free(acon->out);
	g_string_free(acon->in.data, TRUE);

	g_string_free(acon->parse.mod, TRUE);
	g_string_free(acon->parse.action, TRUE);
	g_string_free(acon->parse.error, TRUE);
	g_string_free(acon->parse.data, TRUE);
	close_fd_array(acon->parse.fds);
	g_array_free(acon->parse.fds, TRUE);
	/* TODO */

	g_slice_free(liAngelConnection, acon);
}

static void angel_call_timeout_cb(liEventBase *watcher, int events) {
	liAngelCall* call = LI_CONTAINER_OF(li_event_timer_from(watcher), liAngelCall, timeout_watcher);
	liAngelConnection *acon = call->acon;
	UNUSED(events);

	g_mutex_lock(acon->mutex);
		if (-1 == call->id) {
			/* result available, will be handled in angel_call_result_cb */
			g_mutex_unlock(acon->mutex);
			return;
		}
		g_ptr_array_index(acon->call_table, call->id) = NULL;
		call->id = -1;
	g_mutex_unlock(acon->mutex);

	call->acon = NULL;

	if (NULL != call->callback) {
		call->callback(call->context, TRUE, NULL, NULL, NULL);
	}
	li_angel_call_free(call);
}

static void angel_call_result_cb(liEventBase *watcher, int events) {
	liAngelCall* call = LI_CONTAINER_OF(li_event_async_from(watcher), liAngelCall, result_watcher);
	UNUSED(events);

	LI_FORCE_ASSERT(-1 == call->id);

	call->acon = NULL;

	if (NULL != call->callback) {
		call->callback(call->context, FALSE, call->result.error, call->result.data, call->result.fds);
	}
	li_angel_call_free(call);
}

liAngelCall *li_angel_call_new(liEventLoop *loop, liAngelCallCB callback, li_tstamp timeout) {
	liAngelCall* call = g_slice_new0(liAngelCall);

	g_assert(NULL != callback);
	call->callback = callback;
	li_event_timer_init(loop, &call->timeout_watcher, angel_call_timeout_cb);
	li_event_timer_once(&call->timeout_watcher, timeout);
	li_event_async_init(loop, &call->result_watcher, angel_call_result_cb);
	call->id = -1;

	return call;
}

/* returns TRUE if a call was cancelled */
gboolean li_angel_call_free(liAngelCall *call) {
	gboolean r = FALSE;

	if (NULL != call->acon) {
		liAngelConnection *acon = call->acon;
		g_mutex_lock(acon->mutex);
			if (-1 != call->id) {
				g_ptr_array_index(acon->call_table, call->id) = NULL;
				call->id = -1;
			}
		g_mutex_unlock(acon->mutex);
		r = TRUE;
	}

	li_event_clear(&call->timeout_watcher);
	li_event_clear(&call->result_watcher);

	if (NULL != call->result.error) {
		g_string_free(call->result.error, TRUE);
		call->result.error = NULL;
	}
	if (NULL != call->result.data) {
		g_string_free(call->result.data, TRUE);
		call->result.data = NULL;
	}
	if (NULL != call->result.fds) {
		close_fd_array(call->result.fds);
		g_array_free(call->result.fds, TRUE);
	}

	call->callback = NULL;
	call->context = NULL;

	g_slice_free(liAngelCall, call);

	return r;
}

static gboolean prepare_call_header(GString **pbuf,
		gint32 type, gint32 id,
		const gchar *mod, gsize mod_len, const gchar *action, gsize action_len,
		gsize error_len, gsize data_len, gsize fd_count, GError **err) {
	GString *buf;
	buf = g_string_sized_new(8*4 + mod_len + action_len);
	*pbuf = buf;

	if (!li_angel_data_write_int32(buf, ANGEL_MAGIC, err)) return FALSE;
	if (!li_angel_data_write_int32(buf, type, err)) return FALSE;
	if (!li_angel_data_write_int32(buf, id, err)) return FALSE;
	if (type != ANGEL_CALL_SEND_RESULT) {
		if (!li_angel_data_write_int32(buf, mod_len, err)) return FALSE;
		if (!li_angel_data_write_int32(buf, action_len, err)) return FALSE;
	} else {
		if (!li_angel_data_write_int32(buf, 0, err)) return FALSE;
		if (!li_angel_data_write_int32(buf, 0, err)) return FALSE;
	}
	if (!li_angel_data_write_int32(buf, error_len, err)) return FALSE;
	if (!li_angel_data_write_int32(buf, data_len, err)) return FALSE;
	if (!li_angel_data_write_int32(buf, fd_count, err)) return FALSE;

	if (type != ANGEL_CALL_SEND_RESULT) {
		g_string_append_len(buf, mod, mod_len);
		g_string_append_len(buf, action, action_len);
	}

	return TRUE;
}

gboolean li_angel_send_simple_call(
		liAngelConnection *acon,
		const gchar *mod, gsize mod_len, const gchar *action, gsize action_len,
		GString *data,
		GError **err) {
	GString *buf = NULL;
	gboolean queue_was_empty;

	if (err && *err) goto error;

	if (-1 == acon->fd) {
		g_set_error(err, LI_ANGEL_CONNECTION_ERROR, LI_ANGEL_CONNECTION_CLOSED, "connection already closed");
		goto error;
	}

	if (data && data->len > ANGEL_CALL_MAX_STR_LEN) {
		g_set_error(err, LI_ANGEL_CALL_ERROR, LI_ANGEL_CALL_INVALID, "data too lang for angel call: %" G_GSIZE_FORMAT " > %i", data->len, ANGEL_CALL_MAX_STR_LEN);
		goto error;
	}

	if (!prepare_call_header(&buf, ANGEL_CALL_SEND_SIMPLE, -1, mod, mod_len, action, action_len, 0, data ? data->len : 0, 0, err)) goto error;

	g_mutex_lock(acon->mutex);
		queue_was_empty = (0 == acon->out->length);
		send_queue_push_string(acon->out, buf);
		if (data) send_queue_push_string(acon->out, data);
	g_mutex_unlock(acon->mutex);

	if (queue_was_empty)
		li_event_async_send(&acon->out_notify_watcher);

	return TRUE;

error:
	if (data) g_string_free(data, TRUE);
	if (buf) g_string_free(buf, TRUE);
	return FALSE;
}

gboolean li_angel_send_call(
		liAngelConnection *acon,
		const gchar *mod, gsize mod_len, const gchar *action, gsize action_len,
		liAngelCall *call,
		GString *data,
		GError **err) {
	GString *buf = NULL;
	gboolean queue_was_empty;

	if (err && *err) goto error;

	if (-1 == acon->fd) {
		g_set_error(err, LI_ANGEL_CONNECTION_ERROR, LI_ANGEL_CONNECTION_CLOSED, "connection already closed");
		goto error;
	}

	g_mutex_lock(acon->mutex);
		if (-1 != call->id) {
			g_mutex_unlock(acon->mutex);
			g_set_error(err, LI_ANGEL_CALL_ERROR, LI_ANGEL_CALL_ALREADY_RUNNING, "call already running");
			goto error_before_new_id;
		}

		if (-1 == (call->id = li_idlist_get(acon->call_id_list))) {
			g_mutex_unlock(acon->mutex);
			g_set_error(err, LI_ANGEL_CALL_ERROR, LI_ANGEL_CALL_OUT_OF_CALL_IDS, "out of call ids");
			goto error;
		}
		call->acon = acon;

		if ((guint) call->id >= acon->call_table->len) {
			g_ptr_array_set_size(acon->call_table, call->id + 1);
		}
		g_ptr_array_index(acon->call_table, call->id) = call;
	g_mutex_unlock(acon->mutex);

	if (data && data->len > ANGEL_CALL_MAX_STR_LEN) {
		g_set_error(err, LI_ANGEL_CALL_ERROR, LI_ANGEL_CALL_INVALID, "data too lang for angel call: %" G_GSIZE_FORMAT " > %i", data->len, ANGEL_CALL_MAX_STR_LEN);
		goto error;
	}

	if (!prepare_call_header(&buf, ANGEL_CALL_SEND_CALL, call->id, mod, mod_len, action, action_len, 0, data ? data->len : 0, 0, err)) goto error;

	g_mutex_lock(acon->mutex);
		queue_was_empty = (0 == acon->out->length);
		send_queue_push_string(acon->out, buf);
		if (data) send_queue_push_string(acon->out, data);
	g_mutex_unlock(acon->mutex);

	if (queue_was_empty)
		li_event_async_send(&acon->out_notify_watcher);

	return TRUE;

error:
	if (-1 != call->id) {
		li_idlist_put(acon->call_id_list, call->id);
		call->id = -1;
		call->acon = NULL;
	}
error_before_new_id:
	if (data) g_string_free(data, TRUE);
	if (buf) g_string_free(buf, TRUE);
	return FALSE;
}

gboolean li_angel_send_result(
		liAngelConnection *acon,
		gint32 id,
		GString *error, GString *data, GArray *fds,
		GError **err) {
	GString *buf = NULL;
	gboolean queue_was_empty;

	if (err && *err) goto error;

	if (-1 == acon->fd) {
		g_set_error(err, LI_ANGEL_CONNECTION_ERROR, LI_ANGEL_CONNECTION_CLOSED, "connection already closed");
		goto error;
	}

	if (data && data->len > ANGEL_CALL_MAX_STR_LEN) {
		g_set_error(err, LI_ANGEL_CALL_ERROR, LI_ANGEL_CALL_INVALID, "data too lang for angel call: %" G_GSIZE_FORMAT " > %i", data->len, ANGEL_CALL_MAX_STR_LEN);
		goto error;
	}

	if (!prepare_call_header(&buf, ANGEL_CALL_SEND_RESULT, id, NULL, 0, NULL, 0, error ? error->len : 0, data ? data->len : 0, fds ? fds->len : 0, err)) goto error;

	g_mutex_lock(acon->mutex);
		queue_was_empty = (0 == acon->out->length);
		send_queue_push_string(acon->out, buf);
		send_queue_push_string(acon->out, error);
		if (data) send_queue_push_string(acon->out, data);
		send_queue_push_fds(acon->out, fds);
	g_mutex_unlock(acon->mutex);

	if (queue_was_empty)
		li_event_async_send(&acon->out_notify_watcher);

	return TRUE;

error:
	if (data) g_string_free(data, TRUE);
	if (buf) g_string_free(buf, TRUE);
	if (error) g_string_free(error, TRUE);
	if (fds) close_fd_array(fds);
	return FALSE;

	return FALSE;
}

/* free temporary needed memroy; call this once in while after some activity */
void li_angel_cleanup_tables(liAngelConnection *acon) {
	UNUSED(acon);
	/* TODO
	guint max_used_id = idlist_cleanup(acon->call_id_list);
	g_ptr_array_set_size(acon->call_id_list, max_used_id);
	*/
}

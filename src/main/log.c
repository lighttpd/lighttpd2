/*
	todo:
	- write out logs at startup directly
	- scheme:// prefix
	- LI_LOG_LEVEL_ABORT: write message immediately, as the log write is followed by an abort()

*/

#include <lighttpd/base.h>
#include <lighttpd/plugin_core.h>

#include <stdarg.h>

#define LOG_DEFAULT_TS_FORMAT "%d/%b/%Y %T %Z"
#define LOG_DEFAULT_TTL 30.0

static void log_watcher_cb(liEventBase *watcher, int events);

static void li_log_write_stderr(liServer *srv, const gchar *msg, gboolean newline) {
	gsize s;
	struct tm tm;
	time_t now = (time_t) li_event_time();
	gchar buf[128];

	UNUSED(srv);

#ifdef HAVE_LOCALTIME_R
	s = strftime(buf, sizeof(buf), LOG_DEFAULT_TS_FORMAT, localtime_r(&now, &tm));
#else
	s = strftime(buf, sizeof(buf), LOG_DEFAULT_TS_FORMAT, localtime(&now));
#endif

	buf[s] = '\0';

	g_printerr(newline ? "%s %s\n" : "%s %s", buf, msg);
}

static liLogTarget *log_open(liServer *srv, GString *path) {
	liLogTarget *log;

	log = li_radixtree_lookup_exact(srv->logs.targets, path->str, path->len * 8);

	if (NULL == log) {
		/* log not open */
		gint fd = -1;
		gchar *param = NULL;
		liLogType type = li_log_type_from_path(path, &param);
		GString sparam = { param, (param != NULL ? path->len - (param - path->str) : 0), 0 };

		switch (type) {
			case LI_LOG_TYPE_STDERR:
				fd = STDERR_FILENO;
				break;
			case LI_LOG_TYPE_FILE:
				/* TODO: open via angel */
				fd = li_angel_fake_log_open_file(srv, &sparam);
				break;
			case LI_LOG_TYPE_PIPE:
				ERROR(srv, "%s", "pipe logging not supported yet");
				break;
			case LI_LOG_TYPE_SYSLOG:
				ERROR(srv, "%s", "syslog not supported yet");
				break;
			case LI_LOG_TYPE_NONE:
				return NULL;
		}

		/* Even if -1 == fd we create an entry, so we don't throw an error every time */
		log = g_slice_new0(liLogTarget);
		log->type = type;
		log->path = g_string_new_len(GSTR_LEN(path));
		log->fd = fd;
		log->wqelem.data = log;
		li_radixtree_insert(srv->logs.targets, log->path->str, log->path->len * 8, log);
		/*g_print("log_open(\"%s\")\n", log->path->str);*/
	}

	li_waitqueue_push(&srv->logs.close_queue, &log->wqelem);

	return log;
}

static void log_close(liServer *srv, liLogTarget *log) {
	li_radixtree_remove(srv->logs.targets, log->path->str, log->path->len * 8);
	li_waitqueue_remove(&srv->logs.close_queue, &log->wqelem);

	if (log->type == LI_LOG_TYPE_FILE || log->type == LI_LOG_TYPE_PIPE) {
		if (-1 != log->fd) close(log->fd);
	}

	/*g_print("log_close(\"%s\")\n", log->path->str);*/
	g_string_free(log->path, TRUE);

	g_slice_free(liLogTarget, log);
}

static void log_close_cb(liWaitQueue *wq, gpointer data) {
	/* callback for the close queue */
	liServer *srv = (liServer*) data;
	liWaitQueueElem *wqe;

	while ((wqe = li_waitqueue_pop(wq)) != NULL) {
		log_close(srv, wqe->data);
	}

	li_waitqueue_update(wq);
}

void li_log_init(liServer *srv) {
	li_event_loop_init(&srv->logs.loop, ev_loop_new(EVFLAG_AUTO));
	li_event_async_init(&srv->logs.loop, "log", &srv->logs.watcher, log_watcher_cb);
	srv->logs.targets = li_radixtree_new();
	li_waitqueue_init(&srv->logs.close_queue, &srv->logs.loop, "log close queue", log_close_cb, LOG_DEFAULT_TTL, srv);
	srv->logs.timestamp.format = g_string_new_len(CONST_STR_LEN(LOG_DEFAULT_TS_FORMAT));
	srv->logs.timestamp.cached = g_string_sized_new(255);
	srv->logs.timestamp.last_ts = 0;
	srv->logs.thread_alive = FALSE;
	g_queue_init(&srv->logs.write_queue);
	g_static_mutex_init(&srv->logs.write_queue_mutex);
	srv->logs.log_context.log_map = li_log_map_new_default();
}

void li_log_cleanup(liServer *srv) {
	/* wait for logging thread to exit */
	if (g_atomic_int_get(&srv->logs.thread_alive) == TRUE)
	{
		li_log_thread_finish(srv);
		g_thread_join(srv->logs.thread);
	}

	g_static_mutex_free(&srv->logs.write_queue_mutex);
	li_radixtree_free(srv->logs.targets, NULL, NULL);

	g_string_free(srv->logs.timestamp.format, TRUE);
	g_string_free(srv->logs.timestamp.cached, TRUE);

	ev_loop_destroy(li_event_loop_clear(&srv->logs.loop));

	li_log_context_set(&srv->logs.log_context, NULL);
}

liLogMap* li_log_map_new(void) {
	liLogMap *log_map = g_slice_new0(liLogMap);

	log_map->refcount = 1;

	return log_map;
}

liLogMap* li_log_map_new_default(void) {
	liLogMap *log_map = li_log_map_new();

	/* default: log LI_LOG_LEVEL_WARNING, LI_LOG_LEVEL_ERROR and LI_LOG_LEVEL_BACKEND to stderr */
	log_map->targets[LI_LOG_LEVEL_WARNING] = g_string_new_len(CONST_STR_LEN("stderr"));
	log_map->targets[LI_LOG_LEVEL_ERROR] = g_string_new_len(CONST_STR_LEN("stderr"));
	log_map->targets[LI_LOG_LEVEL_BACKEND] = g_string_new_len(CONST_STR_LEN("stderr"));

	return log_map;
}

void li_log_map_acquire(liLogMap *log_map) {
	LI_FORCE_ASSERT(g_atomic_int_get(&log_map->refcount) > 0);
	g_atomic_int_inc(&log_map->refcount);
}

void li_log_map_release(liLogMap *log_map) {
	if (!log_map) return;

	LI_FORCE_ASSERT(g_atomic_int_get(&log_map->refcount) > 0);
	if (g_atomic_int_dec_and_test(&log_map->refcount)) {
		for (guint i = 0; i < LI_LOG_LEVEL_COUNT; ++i) {
			if (NULL != log_map->targets[i]) {
				g_string_free(log_map->targets[i], TRUE);
			}
		}

		g_slice_free(liLogMap, log_map);
	}
}

void li_log_context_set(liLogContext *context, liLogMap *log_map) {
	if (NULL == context || context->log_map == log_map) return;

	li_log_map_release(context->log_map);
	if (NULL != log_map) {
		li_log_map_acquire(log_map);
		context->log_map = log_map;
	} else {
		context->log_map = NULL;
	}
}

gboolean li_log_write_direct(liServer *srv, liWorker *wrk, GString *path, GString *msg) {
	liLogEntry *log_entry;

	if (!path || path->len == 0) {
		/* ignore empty log targets */
		return TRUE;
	}

	log_entry = g_slice_new(liLogEntry);
	log_entry->path = g_string_new_len(GSTR_LEN(path));
	log_entry->level = 0;
	log_entry->flags = 0;
	log_entry->msg = msg;
	log_entry->queue_link.data = log_entry;
	log_entry->queue_link.next = NULL;
	log_entry->queue_link.prev = NULL;

	if (HEDLEY_LIKELY(wrk)) {
		/* push onto local worker log queue */
		g_queue_push_tail_link(&wrk->logs.log_queue, &log_entry->queue_link);
	} else {
		/* no worker context, push directly onto global log queue */
		g_static_mutex_lock(&srv->logs.write_queue_mutex);
		g_queue_push_tail_link(&srv->logs.write_queue, &log_entry->queue_link);
		g_static_mutex_unlock(&srv->logs.write_queue_mutex);
		li_event_async_send(&srv->logs.watcher);
	}

	return TRUE;
}

gboolean li_log_write(liServer *srv, liWorker *wrk, liLogContext *context, liLogLevel log_level, guint flags, const gchar *fmt, ...) {
	va_list ap;
	GString *log_line;
	liLogEntry *log_entry;
	liLogMap *log_map = NULL;
	GString *path;

	if (!srv) srv = wrk->srv;

	if (NULL != context) log_map = context->log_map;
	if (NULL == log_map) log_map = srv->logs.log_context.log_map;

	if (log_map != NULL && log_level < LI_LOG_LEVEL_COUNT) {
		path = log_map->targets[log_level];
	} else {
		/* no log map or invalid log level */
		return FALSE;
	}

	if (!path || path->len == 0) {
		/* log-level is ignored */
		return TRUE;
	}

	log_line = g_string_sized_new(63);
	va_start(ap, fmt);
	g_string_vprintf(log_line, fmt, ap);
	va_end(ap);

	switch (g_atomic_int_get(&srv->state)) {
	case LI_SERVER_INIT:
	case LI_SERVER_LOADING:
	case LI_SERVER_SUSPENDED:
	case LI_SERVER_WARMUP:
	case LI_SERVER_STOPPING:
	case LI_SERVER_DOWN:
		li_log_write_stderr(srv, log_line->str, TRUE);
		g_string_free(log_line, TRUE);
		return TRUE;
	default:
		break;
	}

	log_entry = g_slice_new(liLogEntry);
	log_entry->path = g_string_new_len(GSTR_LEN(path));
	log_entry->level = log_level;
	log_entry->flags = flags;
	log_entry->msg = log_line;
	log_entry->queue_link.data = log_entry;
	log_entry->queue_link.next = NULL;
	log_entry->queue_link.prev = NULL;

	if (HEDLEY_LIKELY(wrk)) {
		/* push onto local worker log queue */
		g_queue_push_tail_link(&wrk->logs.log_queue, &log_entry->queue_link);
	} else {
		/* no worker context, push directly onto global log queue */
		g_static_mutex_lock(&srv->logs.write_queue_mutex);
		g_queue_push_tail_link(&srv->logs.write_queue, &log_entry->queue_link);
		g_static_mutex_unlock(&srv->logs.write_queue_mutex);
		li_event_async_send(&srv->logs.watcher);
	}

	return TRUE;
}

static gpointer log_thread(liServer *srv) {
	li_event_loop_run(&srv->logs.loop);
	return NULL;
}

static GString *log_timestamp_format(liServer *srv) {
	gsize s;
	struct tm tm;
	time_t now = (time_t) li_event_now(&srv->logs.loop);

	/* cache hit */
	if (now == srv->logs.timestamp.last_ts) {
		return srv->logs.timestamp.cached;
	}

#ifdef HAVE_LOCALTIME_R
	s = strftime(srv->logs.timestamp.cached->str, srv->logs.timestamp.cached->allocated_len, srv->logs.timestamp.format->str, localtime_r(&now, &tm));
#else
	s = strftime(srv->logs.timestamp.cached->str, srv->logs.timestamp.cached->allocated_len, srv->logs.timestamp.format->str, localtime(&now));
#endif

	g_string_set_size(srv->logs.timestamp.cached, s);
	srv->logs.timestamp.last_ts = now;

	return srv->logs.timestamp.cached;
}

static void log_watcher_cb(liEventBase *watcher, int events) {
	liServer *srv = LI_CONTAINER_OF(li_event_async_from(watcher), liServer, logs.watcher);
	GList *queue_link, *queue_link_next;

	UNUSED(events);

	if (g_atomic_int_get(&srv->logs.thread_stop) == TRUE) {
		liWaitQueueElem *wqe;

		while ((wqe = li_waitqueue_pop_force(&srv->logs.close_queue)) != NULL) {
			log_close(srv, wqe->data);
		}
		li_waitqueue_stop(&srv->logs.close_queue);
		li_event_clear(&srv->logs.watcher);
		li_event_loop_end(&srv->logs.loop);
		return;
	}

	/* pop everything from global write queue */
	g_static_mutex_lock(&srv->logs.write_queue_mutex);
	queue_link = g_queue_peek_head_link(&srv->logs.write_queue);
	g_queue_init(&srv->logs.write_queue);
	g_static_mutex_unlock(&srv->logs.write_queue_mutex);

	while (queue_link) {
		liLogTarget *log;
		liLogEntry *log_entry = queue_link->data;
		GString *msg = log_entry->msg;
		gssize bytes_written = 0;
		gssize write_res;

		if (log_entry->flags & LI_LOG_FLAG_TIMESTAMP) {
			GString *ts = log_timestamp_format(srv);
			g_string_prepend_c(msg, ' ');
			g_string_prepend_len(msg, GSTR_LEN(ts));
		}

		li_g_string_append_len(msg, CONST_STR_LEN("\n"));

		log = log_open(srv, log_entry->path);

		if (NULL == log) {
			/* explicit empty target, ignore */
		} else if (-1 == log->fd) {
			/* failed opening log file */
			li_log_write_stderr(srv, msg->str, TRUE);
			goto next;
		}

		/* todo: support for other logtargets than files */
		while (bytes_written < (gssize)msg->len) {
			write_res = write(log->fd, msg->str + bytes_written, msg->len - bytes_written);
			/* write_res = msg->len; */

			/* write() failed, check why */
			if (write_res == -1) {
				GString *str;
				int err = errno;

				switch (err) {
					case EAGAIN:
					case EINTR:
						continue;
				}

				str = g_string_sized_new(63);
				g_string_printf(str, "could not write to log '%s': %s\n", log_entry->path->str, g_strerror(err));
				li_log_write_stderr(srv, str->str, TRUE);
				li_log_write_stderr(srv, msg->str, TRUE);
				break;
			}
			else {
				bytes_written += write_res;
				LI_FORCE_ASSERT(bytes_written <= (gssize) msg->len);
			}
		}

next:
		queue_link_next = queue_link->next;
		g_string_free(log_entry->path, TRUE);
		g_string_free(log_entry->msg, TRUE);
		g_slice_free(liLogEntry, log_entry);
		queue_link = queue_link_next;
	}

	if (g_atomic_int_get(&srv->logs.thread_finish) == TRUE) {
		liWaitQueueElem *wqe;

		while ((wqe = li_waitqueue_pop_force(&srv->logs.close_queue)) != NULL) {
			log_close(srv, wqe->data);
		}
		li_waitqueue_stop(&srv->logs.close_queue);
		li_event_clear(&srv->logs.watcher);
		li_event_loop_end(&srv->logs.loop);
		return;
	}

	return;
}

#define RET(type, offset) do { if (NULL != param) *param = path->str + offset; return type; } while(0)
#define RET_PAR(type, par) do { if (NULL != param) *param = par; return type; } while(0)
#define TRY_SCHEME(scheme, type) do { if (g_str_has_prefix(path->str, scheme)) RET(type, sizeof(scheme)-1); } while(0)
liLogType li_log_type_from_path(GString *path, gchar **param) {
	if (path->len == 0)
		return LI_LOG_TYPE_NONE;

	/* look for scheme: paths */
	TRY_SCHEME("file:", LI_LOG_TYPE_FILE);
	TRY_SCHEME("pipe:", LI_LOG_TYPE_PIPE);
	TRY_SCHEME("stderr:", LI_LOG_TYPE_STDERR);
	TRY_SCHEME("syslog:", LI_LOG_TYPE_SYSLOG);

	/* targets starting with a slash are absolute paths and therefor file targets */
	if (*path->str == '/')
		RET(LI_LOG_TYPE_FILE, 0);

	/* targets starting with a pipe are ... pipes! */
	if (*path->str == '|') {
		guint i = 1;
		while (path->str[i] == ' ') i++; /* skip spaces */
		RET(LI_LOG_TYPE_PIPE, i);
	}

	if (g_str_equal(path->str, "stderr"))
		RET_PAR(LI_LOG_TYPE_STDERR, NULL);

	if (g_str_equal(path->str, "syslog"))
		RET_PAR(LI_LOG_TYPE_SYSLOG, NULL);

	/* fall back to stderr */
	RET_PAR(LI_LOG_TYPE_STDERR, NULL);
}
#undef RET
#undef RET_PAR
#undef TRY_SCHEME

int li_log_level_from_string(GString *str) {
	if (g_str_equal(str->str, "debug"))
		return LI_LOG_LEVEL_DEBUG;
	if (g_str_equal(str->str, "info"))
		return LI_LOG_LEVEL_INFO;
	if (g_str_equal(str->str, "warning"))
		return LI_LOG_LEVEL_WARNING;
	if (g_str_equal(str->str, "error"))
		return LI_LOG_LEVEL_ERROR;
	if (g_str_equal(str->str, "abort"))
		return LI_LOG_LEVEL_ABORT;
	if (g_str_equal(str->str, "backend"))
		return LI_LOG_LEVEL_BACKEND;

	return -1;
}

gchar* li_log_level_str(liLogLevel log_level) {
	switch (log_level) {
	case LI_LOG_LEVEL_DEBUG:   return "debug";
	case LI_LOG_LEVEL_INFO:    return "info";
	case LI_LOG_LEVEL_WARNING: return "warning";
	case LI_LOG_LEVEL_ERROR:   return "error";
	case LI_LOG_LEVEL_ABORT:   return "abort";
	case LI_LOG_LEVEL_BACKEND: return "backend";
	}
	return "unknown";
}

void li_log_thread_start(liServer *srv) {
	GError *err = NULL;
	LI_FORCE_ASSERT(NULL == srv->logs.thread);

	srv->logs.thread = g_thread_create((GThreadFunc)log_thread, srv, TRUE, &err);

	if (srv->logs.thread == NULL) {
		g_printerr("could not create logging thread: %s\n", err->message);
		g_error_free(err);
		abort();
	}

	g_atomic_int_set(&srv->logs.thread_alive, TRUE);
}

void li_log_thread_stop(liServer *srv) {
	if (g_atomic_int_get(&srv->logs.thread_alive) == TRUE) {
		g_atomic_int_set(&srv->logs.thread_stop, TRUE);
		li_log_thread_wakeup(srv);
	}
}

void li_log_thread_finish(liServer *srv) {
	if (g_atomic_int_get(&srv->logs.thread_alive) == TRUE) {
		g_atomic_int_set(&srv->logs.thread_finish, TRUE);
		li_log_thread_wakeup(srv);
	}
}

void li_log_thread_wakeup(liServer *srv) {
	if (!g_atomic_int_get(&srv->logs.thread_alive))
		li_log_thread_start(srv);

	li_event_async_send(&srv->logs.watcher);
}

void li_log_split_lines(liServer *srv, liWorker *wrk, liLogContext *context, liLogLevel log_level, guint flags, gchar *txt, const gchar *prefix) {
	gchar *start;

	start = txt;
	while ('\0' != *txt) {
		if ('\r' == *txt || '\n' == *txt) {
			*txt = '\0';
			if (txt - start > 1) { /* skip empty lines*/
				li_log_write(srv, wrk, context, log_level, flags, "%s%s", prefix, start);
			}
			txt++;
			while (*txt == '\n' || *txt == '\r') txt++;
			start = txt;
		} else {
			txt++;
		}
	}
	if (txt - start > 1) { /* skip empty lines*/
		li_log_write(srv, wrk, context, log_level, flags, "%s%s", prefix, start);
	}
}

void li_log_split_lines_(liServer *srv, liWorker *wrk, liLogContext *context, liLogLevel log_level, guint flags, gchar *txt, const gchar *fmt, ...) {
	va_list ap;
	GString *prefix;

	prefix = g_string_sized_new(0);
	va_start(ap, fmt);
	g_string_vprintf(prefix, fmt, ap);
	va_end(ap);

	li_log_split_lines(srv, wrk, context, log_level, flags, txt, prefix->str);

	g_string_free(prefix, TRUE);
}

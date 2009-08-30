/*
	todo:
	- write out logs at startup directly
	- scheme:// prefix

*/

#include <lighttpd/base.h>
#include <lighttpd/plugin_core.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdarg.h>

void li_log_write(liServer *srv, liLog *log, GString *msg) {
	liLogEntry *log_entry;

	switch (g_atomic_int_get(&srv->state)) {
	case LI_SERVER_INIT:
	case LI_SERVER_LOADING:
	case LI_SERVER_SUSPENDED:
	case LI_SERVER_WARMUP:
	case LI_SERVER_STOPPING:
	case LI_SERVER_DOWN:
		li_angel_log(srv, msg);
		return;
	default:
		break;
	}

	log_ref(srv, log);

	log_entry = g_slice_new(liLogEntry);
	log_entry->log = log;
	log_entry->msg = msg;

	g_async_queue_push(srv->logs.queue, log_entry);
}

gboolean li_log_write_(liServer *srv, liVRequest *vr, liLogLevel log_level, guint flags, const gchar *fmt, ...) {
	va_list ap;
	GString *log_line;
	liLog *log = NULL;
	liLogEntry *log_entry;
	liLogTimestamp *ts = NULL;

	if (vr != NULL) {

		if (!srv) srv = vr->wrk->srv;
		/* get log from connection */
		log = g_array_index(CORE_OPTION(LI_CORE_OPTION_LOG).list, liLog*, log_level);
		if (log == NULL)
			return TRUE;
		ts = CORE_OPTION(LI_CORE_OPTION_LOG_TS_FORMAT).ptr;
		if (!ts)
			ts = g_array_index(srv->logs.timestamps, liLogTimestamp*, 0);
	}
	else {
		log = srv->logs.stderr;
		ts = g_array_index(srv->logs.timestamps, liLogTimestamp*, 0);
	}

	log_ref(srv, log);
	log_line = g_string_sized_new(0);
	va_start(ap, fmt);
	g_string_vprintf(log_line, fmt, ap);
	va_end(ap);

	if (!(flags & LOG_FLAG_NOLOCK))
		log_lock(log);

	if (!(flags & LOG_FLAG_ALLOW_REPEAT)) {

		/* check if last message for this log was the same */
		if (g_string_equal(log->lastmsg, log_line)) {
			log->lastmsg_count++;
			if (!(flags & LOG_FLAG_NOLOCK))
				log_unlock(log);
			log_unref(srv, log);
			g_string_free(log_line, TRUE);
			return TRUE;
		}
		else {
			if (log->lastmsg_count > 0) {
				guint count = log->lastmsg_count;
				log->lastmsg_count = 0;
				li_log_write_(srv, vr, log_level, flags | LOG_FLAG_NOLOCK | LOG_FLAG_ALLOW_REPEAT, "last message repeated %d times", count);
			}
		}
	}

	g_string_assign(log->lastmsg, log_line->str);

	/* for normal error messages, we prepend a timestamp */
	if (flags & LOG_FLAG_TIMESTAMP) {
		time_t cur_ts;

		g_mutex_lock(srv->logs.mutex);

		/* if we have a worker context, we can use its timestamp to save us a call to time() */
		if (vr != NULL)
			cur_ts = (time_t)CUR_TS(vr->wrk);
		else
			cur_ts = time(NULL);

		if (cur_ts != ts->last_ts) {
			gsize s;
			struct tm tm;
			g_string_set_size(ts->cached, 255);
#ifdef HAVE_LOCALTIME_R
			s = strftime(ts->cached->str, ts->cached->allocated_len,
				ts->format->str, localtime_r(&cur_ts, &tm));
#else
			s = strftime(ts->cached->str, ts->cached->allocated_len,
				ts->format->str, localtime(&cur_ts));
#endif

			g_string_set_size(ts->cached, s);

			ts->last_ts = cur_ts;
		}

		g_string_prepend_c(log_line, ' ');
		g_string_prepend_len(log_line, GSTR_LEN(ts->cached));

		g_mutex_unlock(srv->logs.mutex);
	}

	if (!(flags & LOG_FLAG_NOLOCK))
		log_unlock(log);

	g_string_append_len(log_line, CONST_STR_LEN("\r\n"));

	switch (g_atomic_int_get(&srv->state)) {
	case LI_SERVER_INIT:
	case LI_SERVER_LOADING:
	case LI_SERVER_SUSPENDED:
	case LI_SERVER_WARMUP:
	case LI_SERVER_STOPPING:
	case LI_SERVER_DOWN:
		log_unref(srv, log);
		li_angel_log(srv, log_line);
		return TRUE;
	default:
		break;
	}
	log_entry = g_slice_new(liLogEntry);
	log_entry->log = log;
	log_entry->msg = log_line;
	log_entry->level = log_level;

	g_async_queue_push(srv->logs.queue, log_entry);

	/* on critical error, exit */
	if (log_level == LI_LOG_LEVEL_ABORT) {
		log_thread_stop(srv);
		g_atomic_int_set(&srv->exiting, TRUE);
	}

	return TRUE;
}


gpointer log_thread(liServer *srv) {
	liLog *log;
	liLogEntry *log_entry;
	GString *msg;
	gssize bytes_written;
	gssize write_res;

	while (TRUE) {
		if (g_atomic_int_get(&srv->logs.thread_stop) == TRUE)
			break;

		if (g_atomic_int_get(&srv->logs.thread_finish) == TRUE && g_async_queue_length(srv->logs.queue) == 0)
			break;

		log_entry = g_async_queue_pop(srv->logs.queue);

		/* if log_entry->log is NULL, it means that the logger thread has been woken up probably because it should exit */
		if (log_entry->log == NULL) {
			g_slice_free(liLogEntry, log_entry);
			continue;
		}

		log = log_entry->log;
		msg = log_entry->msg;

		bytes_written = 0;

		while (bytes_written < (gssize)msg->len) {
			write_res = write(log->fd, msg->str + bytes_written, msg->len - bytes_written);

			/* write() failed, check why */
			if (write_res == -1) {
				switch (errno) {
					case EAGAIN:
					case EINTR:
						continue;
				}

				g_printerr("could not write to log: %s\n", msg->str);
				break;
			}
			else {
				bytes_written += write_res;
				assert(bytes_written <= (gssize) msg->len);
			}
		}

		g_string_free(msg, TRUE);
		g_slice_free(liLogEntry, log_entry);
		log_unref(srv, log);
	}

	return NULL;
}

void log_rotate(gchar * path, liLog *log, liServer * UNUSED_PARAM(srv)) {

	switch (log->type) {
		case LI_LOG_TYPE_FILE:
			close(log->fd);
			log->fd = open(log->path->str, O_RDWR | O_CREAT | O_APPEND, 0660);
			if (log->fd == -1) {
				g_printerr("failed to reopen log: %s\n", path);
				assert(NULL); /* TODO */
			}
			break;
		case LI_LOG_TYPE_STDERR:
			break;
		case LI_LOG_TYPE_PIPE:
		case LI_LOG_TYPE_SYSLOG:
		case LI_LOG_TYPE_NONE:
			/* TODO */
			assert(NULL);
	}

	g_string_truncate(log->lastmsg, 0);
	log->lastmsg_count = 0;
}

void log_rotate_logs(liServer *srv) {
	UNUSED(srv);

	/*g_atomic_int_set(&srv->rotate_logs, TRUE);*/
}


void log_ref(liServer *srv, liLog *log) {
	UNUSED(srv);
	g_atomic_int_inc(&log->refcount);
}

void log_unref(liServer *srv, liLog *log) {
	g_mutex_lock(srv->logs.mutex);

	if (g_atomic_int_dec_and_test(&log->refcount))
		log_free_unlocked(srv, log);

	g_mutex_unlock(srv->logs.mutex);
}

liLogType log_type_from_path(GString *path) {
	if (path->len == 0)
		return LI_LOG_TYPE_NONE;

	/* look for scheme:// paths */
	if (g_str_has_prefix(path->str, "file://"))
		return LI_LOG_TYPE_FILE;
	if (g_str_has_prefix(path->str, "pipe://"))
		return LI_LOG_TYPE_PIPE;
	if (g_str_has_prefix(path->str, "stderr://"))
		return LI_LOG_TYPE_STDERR;
	if (g_str_has_prefix(path->str, "syslog://"))
		return LI_LOG_TYPE_SYSLOG;

	/* targets starting with a slash are absolute paths and therefor file targets */
	if (*path->str == '/')
		return LI_LOG_TYPE_FILE;

	/* targets starting with a pipe are ... pipes! */
	if (*path->str == '|')
		return LI_LOG_TYPE_PIPE;

	if (g_str_equal(path->str, "stderr"))
		return LI_LOG_TYPE_STDERR;

	if (g_str_equal(path->str, "syslog"))
		return LI_LOG_TYPE_SYSLOG;

	/* fall back to stderr */
	return LI_LOG_TYPE_STDERR;
}

liLogLevel log_level_from_string(GString *str) {
	if (g_str_equal(str->str, "debug"))
		return LI_LOG_LEVEL_DEBUG;
	if (g_str_equal(str->str, "info"))
		return LI_LOG_LEVEL_INFO;
	if (g_str_equal(str->str, "warning"))
		return LI_LOG_LEVEL_WARNING;
	if (g_str_equal(str->str, "error"))
		return LI_LOG_LEVEL_ERROR;
	if (g_str_equal(str->str, "backend"))
		return LI_LOG_LEVEL_BACKEND;

	/* fall back to debug level */
	return LI_LOG_LEVEL_DEBUG;
}

gchar* log_level_str(liLogLevel log_level) {
	switch (log_level) {
		case LI_LOG_LEVEL_DEBUG:	return "debug";
		case LI_LOG_LEVEL_INFO:	return "info";
		case LI_LOG_LEVEL_WARNING:	return "warning";
		case LI_LOG_LEVEL_ERROR:	return "error";
		case LI_LOG_LEVEL_BACKEND:	return "backend";
		default:				return "unknown";
	}
}


liLog *log_new(liServer *srv, liLogType type, GString *path) {
	liLog *log;
	gint fd = -1;

	if (type == LI_LOG_TYPE_NONE)
		return NULL;

	g_mutex_lock(srv->logs.mutex);
	log = g_hash_table_lookup(srv->logs.targets, path->str);

	/* log already open, inc refcount */
	if (log != NULL)
	{
		g_atomic_int_inc(&log->refcount);
		g_mutex_unlock(srv->logs.mutex);
		return log;
	}

	switch (type) {
		case LI_LOG_TYPE_STDERR:
			fd = STDERR_FILENO;
			break;
		case LI_LOG_TYPE_FILE:
			fd = open(path->str, O_RDWR | O_CREAT | O_APPEND, 0660);
			break;
		case LI_LOG_TYPE_PIPE:
		case LI_LOG_TYPE_SYSLOG:
		case LI_LOG_TYPE_NONE:
			/* TODO */
			fd = -1;
			assert(NULL);
	}

	if (fd == -1) {
		g_printerr("failed to open log: %s", g_strerror(errno));
		return NULL;
	}

	log = g_slice_new0(liLog);
	log->lastmsg = g_string_sized_new(0);
	log->fd = fd;
	log->path = g_string_new_len(GSTR_LEN(path));
	log->refcount = 1;
	log->mutex = g_mutex_new();

	g_hash_table_insert(srv->logs.targets, log->path->str, log);

	g_mutex_unlock(srv->logs.mutex);

	return log;
}

/* only call this if srv->logs.mutex is NOT locked */
void log_free(liServer *srv, liLog *log) {
	g_mutex_lock(srv->logs.mutex);
	log_free_unlocked(srv, log);
	g_mutex_unlock(srv->logs.mutex);
}

/* only call this if srv->log_mutex IS locked */
void log_free_unlocked(liServer *srv, liLog *log) {
	if (log->type == LI_LOG_TYPE_FILE || log->type == LI_LOG_TYPE_PIPE)
		close(log->fd);

	g_hash_table_remove(srv->logs.targets, log->path);
	g_string_free(log->path, TRUE);
	g_string_free(log->lastmsg, TRUE);

	g_mutex_free(log->mutex);

	g_slice_free(liLog, log);
}

void log_init(liServer *srv) {
	GString *str;

	srv->logs.targets = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, NULL);
	srv->logs.queue = g_async_queue_new();
	srv->logs.mutex = g_mutex_new();
	srv->logs.timestamps = g_array_new(FALSE, FALSE, sizeof(liLogTimestamp*));
	srv->logs.thread_alive = FALSE;

	/* first entry in srv->logs.timestamps is the default timestamp */
	li_log_timestamp_new(srv, g_string_new_len(CONST_STR_LEN("%d/%b/%Y %T %Z")));

	/* first entry in srv->logs.targets is the plain good old stderr */
	str = g_string_new_len(CONST_STR_LEN("stderr"));
	srv->logs.stderr = log_new(srv, LI_LOG_TYPE_STDERR, str);
	g_string_free(str, TRUE);
}

void log_cleanup(liServer *srv) {
	guint i;
	liLogTimestamp *ts;

	/* wait for logging thread to exit */
	if (g_atomic_int_get(&srv->logs.thread_alive) == TRUE)
	{
		log_thread_finish(srv);
		g_thread_join(srv->logs.thread);
	}

	log_free(srv, srv->logs.stderr);

	g_hash_table_destroy(srv->logs.targets);
	g_mutex_free(srv->logs.mutex);
	g_async_queue_unref(srv->logs.queue);

	for (i = 0; i < srv->logs.timestamps->len; i++) {
		ts = g_array_index(srv->logs.timestamps, liLogTimestamp*, i);
		/* g_print("ts #%d refcount: %d\n", i, ts->refcount); */
		/*if (g_atomic_int_dec_and_test(&ts->refcount)) {
			g_string_free(ts->cached, TRUE);
			g_string_free(ts->format, TRUE);
			g_slice_free(log_timestamp_t, ts);
			g_array_remove_index_fast(srv->logs.timestamps, i);
			i--;
		}*/
	}

	li_log_timestamp_free(srv, g_array_index(srv->logs.timestamps, liLogTimestamp*, 0));

	g_array_free(srv->logs.timestamps, TRUE);
}

void log_thread_start(liServer *srv) {
	GError *err = NULL;

	srv->logs.thread = g_thread_create((GThreadFunc)log_thread, srv, TRUE, &err);
	g_atomic_int_set(&srv->logs.thread_alive, TRUE);

	if (srv->logs.thread == NULL) {
		g_printerr("could not create loggin thread: %s\n", err->message);
		g_error_free(err);
		abort();
	}
}

void log_thread_stop(liServer *srv) {
	if (g_atomic_int_get(&srv->logs.thread_alive) == TRUE) {
		g_atomic_int_set(&srv->logs.thread_stop, TRUE);
		log_thread_wakeup(srv);
	}
}

void log_thread_finish(liServer *srv) {
	if (g_atomic_int_get(&srv->logs.thread_alive) == TRUE) {
		g_atomic_int_set(&srv->logs.thread_finish, TRUE);
		log_thread_wakeup(srv);
	}
}

void log_thread_wakeup(liServer *srv) {
	liLogEntry *e;

	if (!g_atomic_int_get(&srv->logs.thread_alive))
		log_thread_start(srv);

	e = g_slice_new0(liLogEntry);

	g_async_queue_push(srv->logs.queue, e);
}


void log_lock(liLog *log) {
	g_mutex_lock(log->mutex);
}

void log_unlock(liLog *log) {
	g_mutex_unlock(log->mutex);
}

liLogTimestamp *li_log_timestamp_new(liServer *srv, GString *format) {
	liLogTimestamp *ts;

	/* check if there already exists a timestamp entry with the same format */
	for (guint i = 0; i < srv->logs.timestamps->len; i++) {
		ts = g_array_index(srv->logs.timestamps, liLogTimestamp*, i);
		if (g_string_equal(ts->format, format)) {
			g_atomic_int_inc(&(ts->refcount));
			g_string_free(format, TRUE);
			return ts;
		}
	}

	ts = g_slice_new(liLogTimestamp);

	ts->cached = g_string_sized_new(0);
	ts->last_ts = 0;
	ts->refcount = 1;
	ts->format = format;

	g_array_append_val(srv->logs.timestamps, ts);

	return ts;
}

gboolean li_log_timestamp_free(liServer *srv, liLogTimestamp *ts) {
	if (g_atomic_int_dec_and_test(&(ts->refcount))) {
		for (guint i = 0; i < srv->logs.timestamps->len; i++) {
			if (g_string_equal(g_array_index(srv->logs.timestamps, liLogTimestamp*, i)->format, ts->format)) {
				g_array_remove_index_fast(srv->logs.timestamps, i);
				break;
			}
		}
		g_string_free(ts->cached, TRUE);
		g_string_free(ts->format, TRUE);
		g_slice_free(liLogTimestamp, ts);
		return TRUE;
	}

	return FALSE;
}

void li_log_split_lines(liServer *srv, liVRequest *vr, liLogLevel log_level, guint flags, gchar *txt, const gchar *prefix) {
	gchar *start;

	start = txt;
	while ('\0' != *txt) {
		if ('\r' == *txt || '\n' == *txt) {
			*txt = '\0';
			if (txt - start > 1) { /* skip empty lines*/
				li_log_write_(srv, vr, log_level, flags, "%s%s", prefix, start);
			}
			txt++;
			while (*txt == '\n' || *txt == '\r') txt++;
			start = txt;
		} else {
			txt++;
		}
	}
	if (txt - start > 1) { /* skip empty lines*/
		li_log_write_(srv, vr, log_level, flags, "%s%s", prefix, start);
	}
}

void li_log_split_lines_(liServer *srv, liVRequest *vr, liLogLevel log_level, guint flags, gchar *txt, const gchar *fmt, ...) {
	va_list ap;
	GString *prefix;

	prefix = g_string_sized_new(0);
	va_start(ap, fmt);
	g_string_vprintf(prefix, fmt, ap);
	va_end(ap);

	li_log_split_lines(srv, vr, log_level, flags, txt, prefix->str);

	g_string_free(prefix, TRUE);
}

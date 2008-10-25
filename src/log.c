
#include "base.h"
#include "plugin_core.h"
#include <sys/stat.h>
#include <fcntl.h>
#include <stdarg.h>

/* from server.h */

#if REMOVE_PATH_FROM_FILE
const char *remove_path(const char *path) {
	char *p = strrchr(path, DIR_SEPERATOR);
	if (NULL != p && *(p) != '\0') {
		return (p + 1);
	}
	return path;
}
#endif

int log_write(server* UNUSED_PARAM(srv), connection* UNUSED_PARAM(con), const char *fmt, ...) {
	va_list ap;
	GString *logline;

	logline = g_string_sized_new(0);
	va_start(ap, fmt);
	g_string_vprintf(logline, fmt, ap);
	va_end(ap);


	g_string_append_len(logline, CONST_STR_LEN("\r\n"));
	write(STDERR_FILENO, logline->str, logline->len);
	g_string_free(logline, TRUE);

	return 0;
}


gboolean log_write_(server *srv, connection *con, log_level_t log_level, guint flags, const gchar *fmt, ...) {
	va_list ap;
	GString *log_line;
	log_t *log = NULL;
	log_entry_t *log_entry;
	log_timestamp_t *ts = NULL;
	vrequest *vr = con ? con->mainvr : NULL;

	if (con != NULL) {

		if (!srv) srv = con->srv;
		/* get log from connection */
		log = g_array_index(CORE_OPTION(CORE_OPTION_LOG).list, log_t*, log_level);
		if (log == NULL)
			return TRUE;
		ts = CORE_OPTION(CORE_OPTION_LOG_TS_FORMAT).ptr;
		if (!ts)
			ts = g_array_index(srv->logs.timestamps, log_timestamp_t*, 0);
	}
	else {
		log = srv->logs.stderr;
		ts = g_array_index(srv->logs.timestamps, log_timestamp_t*, 0);
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
				log_write_(srv, con, log_level, flags | LOG_FLAG_NOLOCK | LOG_FLAG_ALLOW_REPEAT, "last message repeated %d times", count);
			}
		}
	}

	g_string_assign(log->lastmsg, log_line->str);

	/* for normal error messages, we prepend a timestamp */
	if (flags & LOG_FLAG_TIMETAMP) {
		time_t cur_ts;

		g_mutex_lock(srv->logs.mutex);

		/* if we have a worker context, we can use its timestamp to save us a call to time() */
		if (con != NULL)
			cur_ts = CUR_TS(con->wrk);
		else
			cur_ts = time(NULL);

		if (cur_ts != ts->last_ts) {
			gsize s;
			g_string_set_size(ts->cached, 255);
			s = strftime(ts->cached->str, ts->cached->allocated_len,
				ts->format->str, localtime(&cur_ts));

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


	log_entry = g_slice_new(log_entry_t);
	log_entry->log = log;
	log_entry->msg = log_line;
	log_entry->level = log_level;

	g_async_queue_push(srv->logs.queue, log_entry);

	/* on critical error, exit */
	if (log_level == LOG_LEVEL_ABORT) {
		log_thread_stop(srv);
		g_atomic_int_set(&srv->exiting, TRUE);
	}

	return TRUE;
}


gpointer log_thread(server *srv) {
	GAsyncQueue *queue;
	log_t *log;
	log_entry_t *log_entry;
	GString *msg;
	gssize bytes_written;
	gssize write_res;

	queue = srv->logs.queue;

	while (TRUE) {
		/* do we need to rotate logs? */
		/*
		if (g_atomic_int_get(&srv->rotate_logs)) {
			g_atomic_int_set(&srv->rotate_logs, FALSE);
			g_mutex_lock(srv->logs.mutex);
			g_hash_table_foreach(srv->logs.targets, (GHFunc) log_rotate, srv);
			g_mutex_unlock(srv->logs.mutex);
		}
		*/

		if (g_atomic_int_get(&srv->logs.thread_stop) == TRUE)
			break;

		if (g_atomic_int_get(&srv->logs.thread_finish) == TRUE && g_async_queue_length(srv->logs.queue) == 0)
			break;

		log_entry = g_async_queue_pop(srv->logs.queue);

		/* if log_entry->log is NULL, it means that the logger thread has been woken up probably because it should exit */
		if (log_entry->log == NULL) {
			g_slice_free(log_entry_t, log_entry);
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
		g_slice_free(log_entry_t, log_entry);
		log_unref(srv, log);
	}

	return NULL;
}

void log_rotate(gchar * path, log_t *log, server * UNUSED_PARAM(srv)) {

	switch (log->type) {
		case LOG_TYPE_FILE:
			close(log->fd);
			log->fd = open(log->path->str, O_RDWR | O_CREAT | O_APPEND, 0660);
			if (log->fd == -1) {
				g_printerr("failed to reopen log: %s\n", path);
				assert(NULL); /* TODO */
			}
			break;
		case LOG_TYPE_STDERR:
			break;
		case LOG_TYPE_PIPE:
		case LOG_TYPE_SYSLOG:
			/* TODO */
			assert(NULL);
	}

	g_string_truncate(log->lastmsg, 0);
	log->lastmsg_count = 0;
}

void log_rotate_logs(server *srv) {
	UNUSED(srv);

	/*g_atomic_int_set(&srv->rotate_logs, TRUE);*/
}


void log_ref(server *srv, log_t *log) {
	UNUSED(srv);
	g_atomic_int_inc(&log->refcount);
}

void log_unref(server *srv, log_t *log) {
	g_mutex_lock(srv->logs.mutex);

	if (g_atomic_int_dec_and_test(&log->refcount))
		log_free_unlocked(srv, log);

	g_mutex_unlock(srv->logs.mutex);
}

log_type_t log_type_from_path(GString *path) {
	if (path->len == 0)
		return LOG_TYPE_STDERR;

	/* targets starting with a slash are absolute paths and therefor file targets */
	if (*path->str == '/')
		return LOG_TYPE_FILE;

	/* targets starting with a pipe are ... pipes! */
	if (*path->str == '|')
		return LOG_TYPE_PIPE;

	if (g_str_equal(path->str, "stderr"))
		return LOG_TYPE_STDERR;

	if (g_str_equal(path->str, "syslog"))
		return LOG_TYPE_SYSLOG;

	/* fall back to stderr */
	return LOG_TYPE_STDERR;
}

log_level_t log_level_from_string(GString *str) {
	if (g_str_equal(str->str, "debug"))
		return LOG_LEVEL_DEBUG;
	if (g_str_equal(str->str, "info"))
		return LOG_LEVEL_INFO;
	if (g_str_equal(str->str, "warning"))
		return LOG_LEVEL_WARNING;
	if (g_str_equal(str->str, "error"))
		return LOG_LEVEL_ERROR;

	/* fall back to debug level */
	return LOG_LEVEL_DEBUG;
}

gchar* log_level_str(log_level_t log_level) {
	switch (log_level) {
		case LOG_LEVEL_DEBUG:	return "debug";
		case LOG_LEVEL_INFO:	return "info";
		case LOG_LEVEL_WARNING:	return "warning";
		case LOG_LEVEL_ERROR:	return "error";
		default:				return "unknown";
	}
}


log_t *log_new(server *srv, log_type_t type, GString *path) {
	log_t *log;
	gint fd = -1;

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
		case LOG_TYPE_STDERR:
			fd = STDERR_FILENO;
			break;
		case LOG_TYPE_FILE:
			fd = open(path->str, O_RDWR | O_CREAT | O_APPEND, 0660);
			break;
		case LOG_TYPE_PIPE:
		case LOG_TYPE_SYSLOG:
			/* TODO */
			fd = -1;
			assert(NULL);
	}

	if (fd == -1) {
		g_printerr("failed to open log: %s", g_strerror(errno));
		return NULL;
	}

	log = g_slice_new0(log_t);
	log->lastmsg = g_string_sized_new(0);
	log->fd = fd;
	log->path = path;
	log->refcount = 1;
	log->mutex = g_mutex_new();

	g_hash_table_insert(srv->logs.targets, path->str, log);

	g_mutex_unlock(srv->logs.mutex);

	return log;
}

/* only call this if srv->logs.mutex is NOT locked */
void log_free(server *srv, log_t *log) {
	g_mutex_lock(srv->logs.mutex);
	log_free_unlocked(srv, log);
	g_mutex_unlock(srv->logs.mutex);
}

/* only call this if srv->log_mutex IS locked */
void log_free_unlocked(server *srv, log_t *log) {
	if (log->type == LOG_TYPE_FILE || log->type == LOG_TYPE_PIPE)
		close(log->fd);

	g_hash_table_remove(srv->logs.targets, log->path);
	g_string_free(log->path, TRUE);
	g_string_free(log->lastmsg, TRUE);

	g_mutex_free(log->mutex);

	g_slice_free(log_t, log);
}

void log_init(server *srv) {
	GString *str;

	srv->logs.targets = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, NULL);
	srv->logs.queue = g_async_queue_new();
	srv->logs.mutex = g_mutex_new();
	srv->logs.timestamps = g_array_new(FALSE, FALSE, sizeof(log_timestamp_t*));
	srv->logs.thread_alive = FALSE;

	/* first entry in srv->logs.timestamps is the default timestamp */
	log_timestamp_new(srv, g_string_new_len(CONST_STR_LEN("%d/%b/%Y %T %Z")));

	/* first entry in srv->logs.targets is the plain good old stderr */
	str = g_string_new_len(CONST_STR_LEN("stderr"));
	srv->logs.stderr = log_new(srv, LOG_TYPE_STDERR, str);
}

void log_cleanup(server *srv) {
	guint i;
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

	log_timestamp_t *ts;
	for (i = 0; i < srv->logs.timestamps->len; i++) {
		ts = g_array_index(srv->logs.timestamps, log_timestamp_t*, i);
		g_print("ts #%d refcount: %d\n", i, ts->refcount);
		/*if (g_atomic_int_dec_and_test(&ts->refcount)) {
			g_string_free(ts->cached, TRUE);
			g_string_free(ts->format, TRUE);
			g_slice_free(log_timestamp_t, ts);
			g_array_remove_index_fast(srv->logs.timestamps, i);
			i--;
		}*/
	}

	log_timestamp_free(srv, g_array_index(srv->logs.timestamps, log_timestamp_t*, 0));

	g_array_free(srv->logs.timestamps, TRUE);
}

void log_thread_start(server *srv) {
	GError *err = NULL;

	srv->logs.thread = g_thread_create((GThreadFunc)log_thread, srv, TRUE, &err);
	g_atomic_int_set(&srv->logs.thread_alive, TRUE);

	if (srv->logs.thread == NULL) {
		g_printerr("could not create loggin thread: %s\n", err->message);
		g_error_free(err);
		abort();
	}
}

void log_thread_stop(server *srv) {
	if (g_atomic_int_get(&srv->logs.thread_alive) == TRUE) {
		g_atomic_int_set(&srv->logs.thread_stop, TRUE);
		log_thread_wakeup(srv);
	}
}

void log_thread_finish(server *srv) {
	if (g_atomic_int_get(&srv->logs.thread_alive) == TRUE) {
		g_atomic_int_set(&srv->logs.thread_finish, TRUE);
		log_thread_wakeup(srv);
	}
}

void log_thread_wakeup(server *srv) {
	if (!g_atomic_int_get(&srv->logs.thread_alive))
		log_thread_start(srv);
	log_entry_t *e;

	e = g_slice_new0(log_entry_t);

	g_async_queue_push(srv->logs.queue, e);
}


void log_lock(log_t *log) {
	g_mutex_lock(log->mutex);
}

void log_unlock(log_t *log) {
	g_mutex_unlock(log->mutex);
}

log_timestamp_t *log_timestamp_new(server *srv, GString *format) {
	log_timestamp_t *ts;

	/* check if there already exists a timestamp entry with the same format */
	for (guint i = 0; i < srv->logs.timestamps->len; i++) {
		ts = g_array_index(srv->logs.timestamps, log_timestamp_t*, i);
		if (g_string_equal(ts->format, format)) {
			g_atomic_int_inc(&(ts->refcount));
			g_string_free(format, TRUE);
			return ts;
		}
	}

	ts = g_slice_new(log_timestamp_t);

	ts->cached = g_string_sized_new(0);
	ts->last_ts = 0;
	ts->refcount = 1;
	ts->format = format;

	g_array_append_val(srv->logs.timestamps, ts);

	return ts;
}

gboolean log_timestamp_free(server *srv, log_timestamp_t *ts) {
	if (g_atomic_int_dec_and_test(&(ts->refcount))) {
		for (guint i = 0; i < srv->logs.timestamps->len; i++) {
			if (g_string_equal(g_array_index(srv->logs.timestamps, log_timestamp_t*, i)->format, ts->format)) {
				g_array_remove_index_fast(srv->logs.timestamps, i);
				break;
			}
		}
		g_string_free(ts->cached, TRUE);
		g_string_free(ts->format, TRUE);
		g_slice_free(log_timestamp_t, ts);
		return TRUE;
	}

	return FALSE;
}

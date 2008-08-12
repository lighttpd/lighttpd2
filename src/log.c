
#include "log.h"
#include <stdarg.h>

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


gboolean log_write_(server *srv, connection *con, log_level_t log_level, const gchar *fmt, ...) {
	va_list ap;
	GString *log_line;
	log_t *log;
	log_entry_t *log_entry;
	log_level_t log_level_want;

	if (con != NULL) {
		/* get log index from connection */
		log = con->log;
		log_level_want = con->log_level;
	}
	else {
		log = srv->log_stderr;
		log_level_want = LOG_LEVEL_DEBUG;
	}

	/* ingore messages we are not interested in */
	if (log_level < log_level_want)
		return TRUE;

	log_ref(srv, log);

	log_line = g_string_sized_new(0);
	va_start(ap, fmt);
	g_string_vprintf(log_line, fmt, ap);
	va_end(ap);

	/* check if last message for this log was the same */
	if (g_string_equal(log->lastmsg, log_line)) {
		log->lastmsg_count++;
		log_unref(srv, log);
		g_string_free(log_line, TRUE);
		return TRUE;
	}
	else {
		if (log->lastmsg_count > 0) {
			guint count = log->lastmsg_count;
			log->lastmsg_count = 0;
			log_write_(srv, con, log_level, "last message repeated %d times", count);
		}
	}

	g_string_assign(log->lastmsg, log_line->str);

	g_string_append_len(log_line, CONST_STR_LEN("\r\n"));


	log_entry = g_slice_new(log_entry_t);
	log_entry->log = log;
	log_entry->msg = log_line;
	log_entry->level = log_level;

	g_async_queue_push(srv->log_queue, log_entry);

	/* on critical error, exit */
	if (log_level == LOG_LEVEL_ERROR) {
		g_atomic_int_set(&srv->exiting, TRUE);
		log_thread_wakeup(srv); /* just in case the logging thread is sleeping at this point */
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

	queue = srv->log_queue;

	while (TRUE) {
		/* do we need to rotate logs? */
		if (g_atomic_int_get(&srv->rotate_logs)) {
			g_atomic_int_set(&srv->rotate_logs, FALSE);
			g_mutex_lock(srv->log_mutex);
			g_hash_table_foreach(srv->logs, (GHFunc) log_rotate, srv);
			g_mutex_unlock(srv->log_mutex);
		}

		log_entry = g_async_queue_pop(srv->log_queue);

		if (log_entry->log == NULL) {
			g_slice_free(log_entry_t, log_entry);

			/* lighty is exiting, end logging thread */
			if (g_atomic_int_get(&srv->exiting) && g_async_queue_length(srv->log_queue) == 0)
				break;

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
	g_atomic_int_set(&srv->rotate_logs, TRUE);
}


void log_ref(server *srv, log_t *log) {
	g_mutex_lock(srv->log_mutex);
	log->refcount++;
	g_mutex_unlock(srv->log_mutex);
}

void log_unref(server *srv, log_t *log) {
	g_mutex_lock(srv->log_mutex);

	if (g_atomic_int_dec_and_test(&log->refcount))
		log_free_unlocked(srv, log);

	g_mutex_unlock(srv->log_mutex);
}

log_t *log_new(server *srv, log_type_t type, GString *path) {
	log_t *log;
	gint fd;

	g_mutex_lock(srv->log_mutex);
	log = g_hash_table_lookup(srv->logs, path->str);

	/* log already open, inc refcount */
	if (log != NULL)
	{
		log->refcount++;
		g_mutex_unlock(srv->log_mutex);
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
		g_printerr("failed to open log: %s %d", strerror(errno), errno);
		return NULL;
	}

	log = g_slice_new0(log_t);
	log->lastmsg = g_string_sized_new(0);
	log->fd = fd;
	log->path = path;
	log->refcount = 1;

	g_mutex_unlock(srv->log_mutex);

	return log;
}

/* only call this if srv->log_mutex is NOT locked */
void log_free(server *srv, log_t *log) {
	g_mutex_lock(srv->log_mutex);
	log_free_unlocked(srv, log);
	g_mutex_unlock(srv->log_mutex);
}

/* only call this if srv->log_mutex IS locked */
void log_free_unlocked(server *srv, log_t *log) {
	g_mutex_lock(srv->log_mutex);

	if (log->type == LOG_TYPE_FILE || log->type == LOG_TYPE_PIPE)
		close(log->fd);

	g_hash_table_remove(srv->logs, log->path);
	g_string_free(log->path, TRUE);
	g_string_free(log->lastmsg, TRUE);
	g_slice_free(log_t, log);

	g_mutex_unlock(srv->log_mutex);
}

void log_init(server *srv) {
	GString *str;

	srv->logs = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, NULL);
	srv->log_queue = g_async_queue_new();
	srv->log_mutex = g_mutex_new();

	/* first entry in srv->logs is the plain good old stderr */
	str = g_string_new_len(CONST_STR_LEN("stderr"));
	srv->log_stderr = log_new(srv, LOG_TYPE_STDERR, str);
	srv->log_syslog = NULL;
}

void log_thread_start(server *srv) {
	GError *err = NULL;

	srv->log_thread = g_thread_create((GThreadFunc)log_thread, srv, TRUE, &err);

	if (srv->log_thread == NULL) {
		g_printerr("could not create loggin thread: %s\n", err->message);
		assert(NULL);
	}
}

void log_thread_wakeup(server *srv) {
	log_entry_t *e;

	e = g_slice_new(log_entry_t);
	e->log = NULL;
	e->msg = NULL;

	g_async_queue_push(srv->log_queue, e);
}



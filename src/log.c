
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
		g_mutex_lock(con->mutex);
		log = con->log;
		log_level_want = con->log_level;
		g_mutex_unlock(con->mutex);
	}
	else {
		log = srv->log_stderr;
		log_level_want = LOG_LEVEL_DEBUG;
	}

	/* ingore messages we are not interested in */
	if (log_level < log_level_want)
		return TRUE;

	log_line = g_string_sized_new(0);
	va_start(ap, fmt);
	g_string_vprintf(log_line, fmt, ap);
	va_end(ap);

	/* check if last message for this log was the same */
	if (g_string_equal(log->lastmsg, log_line)) {
		log->lastmsg_count++;
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
	log->lastmsg_fd = log->fd;

	g_string_append_len(log_line, CONST_STR_LEN("\r\n"));


	log_entry = g_slice_new(log_entry_t);
	log_entry->log = log;
	log_entry->msg = log_line;

	log_ref(log);
	g_async_queue_push(srv->log_queue, log_entry);

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
		log_entry = g_async_queue_pop(srv->log_queue);

		/* lighty is exiting, end logging thread */
		if (srv->exiting)
			break;

		if (srv->rotate_logs) {
			g_hash_table_foreach(srv->logs, (GHFunc) log_rotate, NULL);
			srv->rotate_logs = FALSE;
		}

		if (log_entry == NULL)
			continue;

		log = log_entry->log;
		msg = log_entry->msg;

		bytes_written = 0;

		while (bytes_written < (gssize)msg->len) {
			write_res = write(log->fd, msg->str + bytes_written, msg->len - bytes_written);

			assert(write_res <= (gssize) msg->len);

			/* write() failed, check why */
			if (write_res == -1) {
				switch (errno) {
					case EAGAIN:
					case EINTR:
						continue;
				}

				g_printerr("could not write to log: %s\n", msg->str);
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

void log_rotate(gchar * path, log_t *log, gpointer UNUSED_PARAM(user_data)) {
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
}


void log_ref(log_t *log) {
	log->refcount++;
}

void log_unref(server *srv, log_t *log) {
	if (--log->refcount == 0)
		log_free(srv, log);
}

log_t *log_new(server *srv, log_type_t type, GString *path) {
	log_t *log;
	gint fd;

	g_mutex_lock(srv->mutex);
	log = g_hash_table_lookup(srv->logs, path->str);

	/* log already open, inc refcount */
	if (log != NULL)
	{
		log->refcount++;
		g_mutex_unlock(srv->mutex);
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

	g_mutex_unlock(srv->mutex);

	return log;
}

void log_free(server *srv, log_t *log) {
	g_mutex_lock(srv->mutex);

	if (log->type == LOG_TYPE_FILE || log->type == LOG_TYPE_PIPE)
		close(log->fd);

	g_hash_table_remove(srv->logs, log->path);
	g_string_free(log->path, TRUE);
	g_string_free(log->lastmsg, TRUE);
	g_slice_free(log_t, log);

	g_mutex_unlock(srv->mutex);
}

void log_init(server *srv) {
	GError *err = NULL;
	GString *str;

	srv->logs = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, (GDestroyNotify) log_free);
	srv->log_queue = g_async_queue_new();

	/* first entry in srv->logs is the plain good old stderr */
	str = g_string_new_len(CONST_STR_LEN("stderr"));
	srv->log_stderr = log_new(srv, LOG_TYPE_STDERR, str);
	srv->log_syslog = NULL;

	srv->log_thread = g_thread_create((GThreadFunc)log_thread, srv, TRUE, &err);

	if (srv->log_thread == NULL) {
		g_printerr("could not create loggin thread: %s\n", err->message);
		assert(NULL);
	}
}

log_t *log_open_file(const gchar* filename) {
	gint fd;
	log_t *log;


	fd = open(filename, O_RDWR | O_CREAT | O_APPEND, 0660);

	if (fd == -1)
		return NULL;

	log = g_slice_new0(log_t);


	log->fd = fd;
	log->lastmsg = g_string_new("hubba bubba");

	return log;
}



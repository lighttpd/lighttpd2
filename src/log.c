
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
	guint log_ndx;
	log_entry_t *log_entry;
	log_level_t log_level_want;

	if (con != NULL) {
		/* get log index from connection */
		g_mutex_lock(con->mutex);
		log_ndx = con->log_ndx;
		log_level_want = con->log_level;
		g_mutex_unlock(con->mutex);
	}
	else {
		log_ndx = 0;
		log_level_want = LOG_LEVEL_DEBUG;
	}

	/* ingore messages we are not interested in */
	if (log_level_want < log_level)
		return TRUE;

	/* get fd from server */
	g_mutex_lock(srv->mutex);
	log = &g_array_index(srv->logs, log_t, log_ndx);
	g_mutex_unlock(srv->mutex);

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
			log_write_(srv, con, log_level, "last message repeated %d times", log->lastmsg_count);
		}

		log->lastmsg_count = 0;
	}

	g_string_append_len(log_line, CONST_STR_LEN("\r\n"));


	log_entry = g_slice_new(log_entry_t);
	log_entry->fd = log->fd;
	log_entry->msg = log_line;
	g_async_queue_push(srv->log_queue, log_entry);

	return TRUE;
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

void log_free(log_t *log) {
	close(log->fd);
	g_string_free(log->lastmsg, TRUE);
}


gpointer log_thread(server *srv) {
	GAsyncQueue *queue;
	gboolean exiting;
	log_entry_t *log_entry;
	GTimeVal *timeout;
	gssize bytes_written;
	gssize write_res;

	queue = srv->log_queue;

	while (TRUE) {
		/* check if we need to exit */
		g_mutex_lock(srv->mutex);
		exiting = srv->exiting;
		g_mutex_unlock(srv->mutex);
		if (exiting)
			break;

		/* 1 second timeout */
		g_get_current_time(timeout);
		g_time_val_add(timeout, 1000 * 1000 * 1);
		log_entry = g_async_queue_timed_pop(srv->log_queue, timeout);

		g_print("log_thread ping\n");

		if (log_entry == NULL)
			continue;

		while (bytes_written < (gssize)log_entry->msg->len) {
			write_res = write(log_entry->fd, log_entry->msg->str + bytes_written, log_entry->msg->len - bytes_written);

			assert(write_res <= (gssize) log_entry->msg->len);

			/* write() failed, check why */
			if (write_res == -1) {
				switch (errno) {
					case EAGAIN:
					case EINTR:
						continue;
				}
			}
			else {
				bytes_written += write_res;
				assert(bytes_written <= (gssize) log_entry->msg->len);
			}
		}

		g_string_free(log_entry->msg, TRUE);
		g_slice_free(log_entry_t, log_entry);
	}

	return NULL;
}

void log_init(server *srv) {
	log_t *log;
	GError *err = NULL;

	srv->log_thread = g_thread_create((GThreadFunc)log_thread, srv, TRUE, &err);

	if (srv->log_thread == NULL) {
		g_printerr("could not create loggin thread: %s\n", err->message);
		assert(NULL);
	}


	/* first entry in srv->logs is the plain good old stderr */
	log = g_slice_new0(log_t);
	log->fd = STDERR_FILENO;
	log->lastmsg = g_string_sized_new(0);
	g_array_append_val(srv->logs, log);
}



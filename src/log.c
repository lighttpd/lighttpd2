
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


gboolean log_write_(server *srv, connection *con, const char *fmt, ...) {
	va_list ap;
	GString *log_line;
	static GStaticMutex log_mutex = G_STATIC_MUTEX_INIT;
	gint log_fd;
	gssize bytes_written;
	gssize write_res;

	if (con != NULL) {
		/* get fd from connection */
		g_mutex_lock(con->mutex);
		log_fd = con->error_log_fd;
		g_mutex_unlock(con->mutex);
	}
	else {
		/* get fd from server */
		g_mutex_lock(srv->mutex);
		log_fd = srv->error_log_fd;
		g_mutex_unlock(srv->mutex);
	}

	log_line = g_string_sized_new(0);
	va_start(ap, fmt);
	g_string_vprintf(log_line, fmt, ap);
	va_end(ap);

	g_string_append_len(log_line, CONST_STR_LEN("\r\n"));

	bytes_written = 0;

	/* lock to ensure that multiple threads don't mess up the logs */
	g_static_mutex_lock(&log_mutex);
	while (bytes_written < (gssize)log_line->len) {
		write_res = write(log_fd, log_line->str + bytes_written, log_line->len - bytes_written);

		assert(write_res <= (gssize) log_line->len);

		/* write() failed, check why */
		if (write_res == -1) {
			switch (errno) {
				case EAGAIN:
				case EINTR:
					continue;
			}

			/* the error is serious, unlock mutex and return as we can't seem to write to the log */
			g_static_mutex_unlock(&log_mutex);
			g_string_free(log_line, TRUE);
			return FALSE;
		}
		else {
			bytes_written += write_res;
			assert(bytes_written <= (gssize) log_line->len);
		}
	}

	g_static_mutex_unlock(&log_mutex);
	g_string_free(log_line, TRUE);

	return TRUE;
}

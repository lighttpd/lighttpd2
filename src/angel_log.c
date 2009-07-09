
#include <lighttpd/angel_base.h>

#if LI_REMOVE_PATH_FROM_FILE
const char *li_remove_path(const char *path) {
	char *p = strrchr(path, DIR_SEPERATOR);
	if (NULL != p && *(p) != '\0') {
		return (p + 1);
	}
	return path;
}
#endif

void log_init(liServer *srv) {
	srv->log.type = LI_LOG_TYPE_STDERR;

	srv->log.levels[LI_LOG_LEVEL_ABORT] = TRUE;
	srv->log.levels[LI_LOG_LEVEL_ERROR] = TRUE;
	srv->log.levels[LI_LOG_LEVEL_WARNING] = TRUE;

	srv->log.levels[LI_LOG_LEVEL_INFO] = TRUE; /* TODO: remove debug levels */
	srv->log.levels[LI_LOG_LEVEL_DEBUG] = TRUE;

	srv->log.fd = -1;
	srv->log.ts_cache = g_string_sized_new(0);
	srv->log.log_line = g_string_sized_new(0);
}

void log_clean(liServer *srv) {
	g_string_free(srv->log.ts_cache, TRUE);
	g_string_free(srv->log.log_line, TRUE);
}

void li_log_write(liServer *srv, liLogLevel log_level, guint flags, const gchar *fmt, ...) {
	va_list ap;
	GString *log_line = srv->log.log_line;

	if (!srv->log.levels[log_level]) return;

	g_string_truncate(log_line, 0);

	/* for normal error messages, we prepend a timestamp */
	if (flags & LI_LOG_FLAG_TIMESTAMP) {
		GString *log_ts = srv->log.ts_cache;
		time_t cur_ts;

		cur_ts = (time_t)ev_now(srv->loop);

		if (cur_ts != srv->log.last_ts) {
			gsize s;
			g_string_set_size(log_ts, 255);
			s = strftime(log_ts->str, log_ts->allocated_len, "%Y-%m-%d %H:%M:%S %Z: ", localtime(&cur_ts));

			g_string_set_size(log_ts, s);

			srv->log.last_ts = cur_ts;
		}

		g_string_append_len(log_line, GSTR_LEN(log_ts));
	}

	va_start(ap, fmt);
	g_string_append_vprintf(log_line, fmt, ap);
	va_end(ap);

	g_string_append_len(log_line, CONST_STR_LEN("\n"));

	fprintf(stderr, "%s", log_line->str);
}

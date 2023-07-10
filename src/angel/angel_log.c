
#include <lighttpd/angel_base.h>

void li_log_init(liServer *srv) {
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

void li_log_clean(liServer *srv) {
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
		time_t li_cur_ts;

		li_cur_ts = (time_t) (li_event_now(&srv->loop));

		if (li_cur_ts != srv->log.last_ts) {
			gsize s;
			struct tm tm;

			g_string_set_size(log_ts, 255);
#ifdef HAVE_LOCALTIME_R
			s = strftime(log_ts->str, log_ts->allocated_len, "%Y-%m-%d %H:%M:%S %Z: ", localtime_r(&li_cur_ts, &tm));
#else
			s = strftime(log_ts->str, log_ts->allocated_len, "%Y-%m-%d %H:%M:%S %Z: ", localtime(&li_cur_ts));
#endif

			g_string_set_size(log_ts, s);

			srv->log.last_ts = li_cur_ts;
		}

		li_g_string_append_len(log_line, GSTR_LEN(log_ts));
	}

	va_start(ap, fmt);
	g_string_append_vprintf(log_line, fmt, ap);
	va_end(ap);

	li_g_string_append_len(log_line, CONST_STR_LEN("\n"));

	fprintf(stderr, "%s", log_line->str);
}

void li_log_split_lines(liServer *srv, liLogLevel log_level, guint flags, gchar *txt, const gchar *prefix) {
	gchar *start;

	start = txt;
	while ('\0' != *txt) {
		if ('\r' == *txt || '\n' == *txt) {
			*txt = '\0';
			if (txt - start > 1) { /* skip empty lines*/
				li_log_write(srv, log_level, flags, "%s%s", prefix, start);
			}
			txt++;
			while (*txt == '\n' || *txt == '\r') txt++;
			start = txt;
		} else {
			txt++;
		}
	}
	if (txt - start > 1) { /* skip empty lines*/
		li_log_write(srv, log_level, flags, "%s%s", prefix, start);
	}
}

void li_log_split_lines_(liServer *srv, liLogLevel log_level, guint flags, gchar *txt, const gchar *fmt, ...) {
	va_list ap;
	GString *prefix;

	prefix = g_string_sized_new(0);
	va_start(ap, fmt);
	g_string_vprintf(prefix, fmt, ap);
	va_end(ap);

	li_log_split_lines(srv, log_level, flags, txt, prefix->str);

	g_string_free(prefix, TRUE);
}

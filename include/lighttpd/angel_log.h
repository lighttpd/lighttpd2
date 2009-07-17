#ifndef _LIGHTTPD_ANGEL_LOG_H_
#define _LIGHTTPD_ANGEL_LOG_H_

#ifndef _LIGHTTPD_ANGEL_BASE_H_
#error Please include <lighttpd/angel_base.h> instead of this file
#endif

/* #include <lighttpd/valgrind/valgrind.h> */

#define SEGFAULT(srv, fmt, ...) \
	do { \
		li_log_write_(srv, LI_LOG_LEVEL_ABORT, LI_LOG_FLAG_TIMESTAMP, "(crashing) %s.%d: "fmt, LI_REMOVE_PATH(__FILE__), __LINE__, __VA_ARGS__); \
		/* VALGRIND_PRINTF_BACKTRACE(fmt, __VA_ARGS__); */\
		abort();\
	} while(0)

#define ERROR(srv, fmt, ...) \
	li_log_write(srv, LI_LOG_LEVEL_ERROR, LI_LOG_FLAG_TIMESTAMP, "error (%s:%d): "fmt, LI_REMOVE_PATH(__FILE__), __LINE__, __VA_ARGS__)

#define WARNING(srv, fmt, ...) \
	li_log_write(srv, LI_LOG_LEVEL_WARNING, LI_LOG_FLAG_TIMESTAMP, "warning (%s:%d): "fmt, LI_REMOVE_PATH(__FILE__), __LINE__, __VA_ARGS__)

#define INFO(srv, fmt, ...) \
	li_log_write(srv, LI_LOG_LEVEL_INFO, LI_LOG_FLAG_TIMESTAMP, "info (%s:%d): "fmt, LI_REMOVE_PATH(__FILE__), __LINE__, __VA_ARGS__)

#define DEBUG(srv, fmt, ...) \
	li_log_write(srv, LI_LOG_LEVEL_DEBUG, LI_LOG_FLAG_TIMESTAMP, "debug (%s:%d): "fmt, LI_REMOVE_PATH(__FILE__), __LINE__, __VA_ARGS__)

/* log messages from lighty always as ERROR */
#define INSTANCE(srv, inst, msg) \
	li_log_write(srv, LI_LOG_LEVEL_ERROR, LI_LOG_FLAG_NONE, "lighttpd[%d]: %s", (int) inst->pid, msg)

#define GERROR(srv, error, fmt, ...) \
	li_log_write_(srv, LI_LOG_LEVEL_ERROR, LOG_FLAG_TIMESTAMP, "error (%s:%d): " fmt "\n  %s", LI_REMOVE_PATH(__FILE__), __LINE__, __VA_ARGS__, error ? error->message : "Empty GError")

typedef enum {
	LI_LOG_LEVEL_DEBUG,
	LI_LOG_LEVEL_INFO,
	LI_LOG_LEVEL_WARNING,
	LI_LOG_LEVEL_ERROR,
	LI_LOG_LEVEL_ABORT
} liLogLevel;

#define LI_LOG_LEVEL_COUNT (LI_LOG_LEVEL_ABORT+1)

typedef enum {
	LI_LOG_TYPE_STDERR,
	LI_LOG_TYPE_FILE,
	LI_LOG_TYPE_PIPE,
	LI_LOG_TYPE_SYSLOG,
	LI_LOG_TYPE_NONE
} liLogType;

#define LI_LOG_FLAG_NONE         (0x0)      /* default flag */
#define LI_LOG_FLAG_TIMESTAMP    (0x1)      /* prepend a timestamp to the log message */

typedef struct liLog liLog;

struct liLog {
	liLogType type;
	gboolean levels[LI_LOG_LEVEL_COUNT];
	GString *path;
	gint fd;

	time_t last_ts;
	GString *ts_cache;

	GString *log_line;
};

void log_init(liServer *srv);
void log_clean(liServer *srv);

LI_API void li_log_write(liServer *srv, liLogLevel log_level, guint flags, const gchar *fmt, ...) G_GNUC_PRINTF(4, 5);

#endif

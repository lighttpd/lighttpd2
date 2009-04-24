#ifndef _LIGHTTPD_ANGEL_LOG_H_
#define _LIGHTTPD_ANGEL_LOG_H_

#ifndef _LIGHTTPD_ANGEL_BASE_H_
#error Please include <lighttpd/angel_base.h> instead of this file
#endif

/* #include <lighttpd/valgrind/valgrind.h> */

#define REMOVE_PATH_FROM_FILE 1
#if REMOVE_PATH_FROM_FILE
LI_API const char *remove_path(const char *path);
#define REMOVE_PATH(file) remove_path(file)
#else
#define REMOVE_PATH(file) file
#endif

#define SEGFAULT(srv, fmt, ...) \
	do { \
		log_write_(srv, LOG_LEVEL_ABORT, LOG_FLAG_TIMESTAMP, "(crashing) %s.%d: "fmt, REMOVE_PATH(__FILE__), __LINE__, __VA_ARGS__); \
		/* VALGRIND_PRINTF_BACKTRACE(fmt, __VA_ARGS__); */\
		abort();\
	} while(0)

#define ERROR(srv, fmt, ...) \
	log_write(srv, LOG_LEVEL_ERROR, LOG_FLAG_TIMESTAMP, "error (%s:%d): "fmt, REMOVE_PATH(__FILE__), __LINE__, __VA_ARGS__)

#define WARNING(srv, fmt, ...) \
	log_write(srv, LOG_LEVEL_WARNING, LOG_FLAG_TIMESTAMP, "warning (%s:%d): "fmt, REMOVE_PATH(__FILE__), __LINE__, __VA_ARGS__)

#define INFO(srv, fmt, ...) \
	log_write(srv, LOG_LEVEL_INFO, LOG_FLAG_TIMESTAMP, "info (%s:%d): "fmt, REMOVE_PATH(__FILE__), __LINE__, __VA_ARGS__)

#define DEBUG(srv, fmt, ...) \
	log_write(srv, LOG_LEVEL_DEBUG, LOG_FLAG_TIMESTAMP, "debug (%s:%d): "fmt, REMOVE_PATH(__FILE__), __LINE__, __VA_ARGS__)

/* log messages from lighty always as ERROR */
#define INSTANCE(srv, inst, msg) \
	log_write(srv, LOG_LEVEL_ERROR, LOG_FLAG_NONE, "lighttpd[%d]: %s", (int) inst->pid, msg)

typedef enum {
	LOG_LEVEL_DEBUG,
	LOG_LEVEL_INFO,
	LOG_LEVEL_WARNING,
	LOG_LEVEL_ERROR,
	LOG_LEVEL_ABORT
} log_level_t;

#define LOG_LEVEL_COUNT (LOG_LEVEL_ABORT+1)

typedef enum {
	LOG_TYPE_STDERR,
	LOG_TYPE_FILE,
	LOG_TYPE_PIPE,
	LOG_TYPE_SYSLOG,
	LOG_TYPE_NONE
} log_type_t;

#define LOG_FLAG_NONE         (0x0)      /* default flag */
#define LOG_FLAG_TIMESTAMP    (0x1)      /* prepend a timestamp to the log message */

struct log_t;
typedef struct log_t log_t;

struct log_t {
	log_type_t type;
	gboolean levels[LOG_LEVEL_COUNT];
	GString *path;
	gint fd;

	time_t last_ts;
	GString *ts_cache;

	GString *log_line;
};

void log_init(server *srv);
void log_clean(server *srv);

LI_API void log_write(server *srv, log_level_t log_level, guint flags, const gchar *fmt, ...) G_GNUC_PRINTF(4, 5);

#endif

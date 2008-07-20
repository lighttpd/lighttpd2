#ifndef _LIGHTTPD_LOG_H_
#define _LIGHTTPD_LOG_H_

/* #include "valgrind/valgrind.h" */
#include "base.h"
#include "ev.h"

#define REMOVE_PATH_FROM_FILE 1
#if REMOVE_PATH_FROM_FILE
LI_API const char *remove_path(const char *path);
#define REMOVE_PATH(file) remove_path(file)
#else
#define REMOVE_PATH(file) file
#endif


#define ERROR(srv, fmt, ...) \
	log_write(srv, NULL, "%s.%d: (error) "fmt, REMOVE_PATH(__FILE__), __LINE__, __VA_ARGS__)

#define TRACE(srv, fmt, ...) \
	log_write(srv, NULL, "%s.%d: (trace) "fmt, REMOVE_PATH(__FILE__), __LINE__, __VA_ARGS__)

#define SEGFAULT(srv, fmt, ...) \
	do { \
		log_write(srv, NULL, "%s.%d: (crashing) "fmt, REMOVE_PATH(__FILE__), __LINE__, __VA_ARGS__); \
		/* VALGRIND_PRINTF_BACKTRACE(fmt, __VA_ARGS__); */\
		abort();\
	} while(0)

#define CON_ERROR(srv, con, fmt, ...) \
	log_write(srv, con, "%s.%d: (error) "fmt, REMOVE_PATH(__FILE__), __LINE__, __VA_ARGS__)

#define CON_TRACE(srv, con, fmt, ...) \
	log_write(srv, con, "%s.%d: (trace) "fmt, REMOVE_PATH(__FILE__), __LINE__, __VA_ARGS__)

#define CON_SEGFAULT(srv, con, fmt, ...) \
	do { \
		log_write(srv, con, "%s.%d: (crashing) "fmt, REMOVE_PATH(__FILE__), __LINE__, __VA_ARGS__); \
		/* VALGRIND_PRINTF_BACKTRACE(fmt, __VA_ARGS__); */ \
		abort();\
	} while(0)


/* TODO: perhaps make portable (detect if cc supports) */
#define	__ATTRIBUTE_PRINTF_FORMAT(fmt, arg) __attribute__ ((__format__ (__printf__, fmt, arg)))

LI_API int log_write(server *srv, connection *con, const char *fmt, ...) __ATTRIBUTE_PRINTF_FORMAT(3, 4);



/* convenience makros */
#define log_error(srv, con, fmt, ...) \
	log_write_(srv, con, LOG_LEVEL_ERROR, fmt, __VA_ARGS__)

#define log_warning(srv, con, fmt, ...) \
	log_write_(srv, con, LOG_LEVEL_WARNING, fmt, __VA_ARGS__)

#define log_info(srv, con, fmt, ...) \
	log_write_(srv, con, LOG_LEVEL_INFO, fmt, __VA_ARGS__)

#define log_message(srv, con, fmt, ...) \
	log_write_(srv, con, LOG_LEVEL_MESSAGE, fmt, __VA_ARGS__)

#define log_debug(srv, con, fmt, ...) \
	log_write_(srv, con, LOG_LEVEL_DEBUG, fmt, __VA_ARGS__)



struct log_t;
typedef struct log_t log_t;

struct log_entry_t;
typedef struct log_entry_t log_entry_t;

typedef enum {
	LOG_LEVEL_DEBUG,
	LOG_LEVEL_INFO,
	LOG_LEVEL_MESSAGE,
	LOG_LEVEL_WARNING,
	LOG_LEVEL_ERROR
} log_level_t;

typedef enum {
	LOG_TYPE_STDERR,
	LOG_TYPE_FILE,
	LOG_TYPE_PIPE,
	LOG_TYPE_SYSLOG
} log_type_t;

struct log_t {
	log_type_t type;
	GString *path;
	guint refcount;
	gint fd;
	GString *lastmsg;
	guint lastmsg_count;
	GMutex *mutex;
};

struct log_entry_t {
	log_t *log;
	GString *msg;
};


log_t *log_new(server *srv, log_type_t type, GString *path);
void log_free(server *srv, log_t *log);

void log_ref(log_t *log);
void log_unref(server *srv, log_t *log);

void log_rotate(gchar *path, log_t *log, server *srv);

gpointer log_thread(server *srv);
void log_thread_start(server *srv);
void log_thread_wakeup(server *srv);

void log_init(server *srv);
gboolean log_write_(server *srv, connection *con, log_level_t log_level, const gchar *fmt, ...);

#endif

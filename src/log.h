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


#define ERROR(fmt, ...) \
	log_write(NULL, NULL, "%s.%d: (error) "fmt, REMOVE_PATH(__FILE__), __LINE__, __VA_ARGS__)

#define TRACE(fmt, ...) \
	log_write(NULL, NULL, "%s.%d: (trace) "fmt, REMOVE_PATH(__FILE__), __LINE__, __VA_ARGS__)

#define SEGFAULT(fmt, ...) \
	do { \
		log_write(NULL, NULL, "%s.%d: (crashing) "fmt, REMOVE_PATH(__FILE__), __LINE__, __VA_ARGS__); \
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

struct log_t;
typedef struct log_t log_t;

struct log_t {
	gint fd;
	GMutex *mutex;
	GString *lastmsg;
	guint lastmsg_count;
};

log_t *log_new(const gchar* filename);
void log_free(log_t *log);

#endif

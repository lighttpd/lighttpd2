#ifndef _LIGHTTPD_LOG_H_
#define _LIGHTTPD_LOG_H_

#ifndef _LIGHTTPD_BASE_H_
#error Please include <lighttpd/base.h> instead of this file
#endif

/*
 * Logging uses a dedicated thread in order to prevent blocking write io from blocking normal operations in worker threads.
 * Code handling vrequests should use the VR_ERROR(), VR_DEBUG() etc macros. Otherwise the ERROR(), DEBUG() etc macros should be used.
 * Basic examples: VR_WARNING(vr, "%s", "something unexpected happened")   ERROR(srv, "%d is not bigger than %d", 23, 42)
 *
 * Log targets specify where the log messages are written to. They are kept open for a certain amount of time (default 30s).
 * file://
 *
 * Logs are sent once per event loop iteration to the logging thread in order to reduce syscalls and lock contention.
 */

/* at least one of srv and wrk must not be NULL. ctx may be NULL. */
#define _SEGFAULT(srv, wrk, ctx, fmt, ...) \
	do { \
		li_log_write(srv, NULL, NULL, LI_LOG_LEVEL_ABORT, LI_LOG_FLAG_TIMESTAMP, "(crashing) %s:%d: %s " fmt, LI_REMOVE_PATH(__FILE__), __LINE__, G_STRFUNC, __VA_ARGS__); \
		li_print_backtrace_stderr(); \
		abort(); \
	} while(0)

#define _ERROR(srv, wrk, ctx, fmt, ...) \
	li_log_write(srv, wrk, ctx, LI_LOG_LEVEL_ERROR, LI_LOG_FLAG_TIMESTAMP, "(error) %s:%d: " fmt, LI_REMOVE_PATH(__FILE__), __LINE__, __VA_ARGS__)

#define _WARNING(srv, wrk, ctx, fmt, ...) \
	li_log_write(srv, wrk, ctx, LI_LOG_LEVEL_WARNING, LI_LOG_FLAG_TIMESTAMP, "(warning) %s:%d: " fmt, LI_REMOVE_PATH(__FILE__), __LINE__, __VA_ARGS__)

#define _INFO(srv, wrk, ctx, fmt, ...) \
	li_log_write(srv, wrk, ctx, LI_LOG_LEVEL_INFO, LI_LOG_FLAG_TIMESTAMP, "(info) %s:%d: " fmt, LI_REMOVE_PATH(__FILE__), __LINE__, __VA_ARGS__)

#define _DEBUG(srv, wrk, ctx, fmt, ...) \
	li_log_write(srv, wrk, ctx, LI_LOG_LEVEL_DEBUG, LI_LOG_FLAG_TIMESTAMP, "(debug) %s:%d: " fmt, LI_REMOVE_PATH(__FILE__), __LINE__, __VA_ARGS__)

#define _BACKEND(srv, wrk, ctx, fmt, ...) \
	li_log_write(srv, wrk, ctx, LI_LOG_LEVEL_BACKEND, LI_LOG_FLAG_TIMESTAMP, fmt, __VA_ARGS__)
#define _BACKEND_LINES(srv, wrk, ctx, txt, fmt, ...) \
	li_log_split_lines_(srv, wrk, ctx, LI_LOG_LEVEL_BACKEND, LI_LOG_FLAG_TIMESTAMP, txt, fmt, __VA_ARGS__)

#define _GERROR(srv, wrk, ctx, error, fmt, ...) \
	li_log_write(srv, wrk, ctx, LI_LOG_LEVEL_ERROR, LI_LOG_FLAG_TIMESTAMP, "(error) %s:%d: " fmt "\n  %s", LI_REMOVE_PATH(__FILE__), __LINE__, __VA_ARGS__, error ? error->message : "Empty GError")

#define VR_SEGFAULT(vr, fmt, ...) _SEGFAULT(vr->wrk->srv, vr->wrk, &vr->log_context, fmt, __VA_ARGS__)
#define VR_ERROR(vr, fmt, ...)    _ERROR(vr->wrk->srv, vr->wrk, &vr->log_context, fmt, __VA_ARGS__)
#define VR_WARNING(vr, fmt, ...)  _WARNING(vr->wrk->srv, vr->wrk, &vr->log_context, fmt, __VA_ARGS__)
#define VR_INFO(vr, fmt, ...)     _INFO(vr->wrk->srv, vr->wrk, &vr->log_context, fmt, __VA_ARGS__)
#define VR_DEBUG(vr, fmt, ...)    _DEBUG(vr->wrk->srv, vr->wrk, &vr->log_context, fmt, __VA_ARGS__)
#define VR_BACKEND(vr, fmt, ...)  _BACKEND(vr->wrk->srv, vr->wrk, &vr->log_context, fmt, __VA_ARGS__)
#define VR_BACKEND_LINES(vr, txt, fmt, ...) _BACKEND_LINES(vr->wrk->srv, vr->wrk, &vr->log_context, txt, fmt, __VA_ARGS__)
#define VR_GERROR(vr, error, fmt, ...) _GERROR(vr->wrk->srv, vr->wrk, &vr->log_context, error, fmt, __VA_ARGS__)

/* vr may be NULL; if vr is NULL, srv must NOT be NULL */
#define _VR_SEGFAULT(srv, vr, fmt, ...) _SEGFAULT(srv, NULL != vr ? vr->wrk : NULL, &vr->log_context, fmt, __VA_ARGS__)
#define _VR_ERROR(srv, vr, fmt, ...)    _ERROR(srv, NULL != vr ? vr->wrk : NULL, &vr->log_context, fmt, __VA_ARGS__)
#define _VR_WARNING(srv, vr, fmt, ...)  _WARNING(srv, NULL != vr ? vr->wrk : NULL, &vr->log_context, fmt, __VA_ARGS__)
#define _VR_INFO(srv, vr, fmt, ...)     _INFO(srv, NULL != vr ? vr->wrk : NULL, &vr->log_context, fmt, __VA_ARGS__)
#define _VR_DEBUG(srv, vr, fmt, ...)    _DEBUG(srv, NULL != vr ? vr->wrk : NULL, &vr->log_context, fmt, __VA_ARGS__)
#define _VR_BACKEND(srv, vr, fmt, ...)  _BACKEND(srv, NULL != vr ? vr->wrk : NULL, &vr->log_context, fmt, __VA_ARGS__)
#define _VR_BACKEND_LINES(srv, vr, txt, fmt, ...) _BACKEND_LINES(srv, NULL != vr ? vr->wrk : NULL, &vr->log_context, txt, fmt, __VA_ARGS__)
#define _VR_GERROR(srv, vr, error, fmt, ...) _GERROR(srv, NULL != vr ? vr->wrk : NULL, &vr->log_context, error, fmt, __VA_ARGS__)

#define SEGFAULT(srv, fmt, ...)   _SEGFAULT(srv, NULL, NULL, fmt, __VA_ARGS__)
#define ERROR(srv, fmt, ...)      _ERROR(srv, NULL, NULL, fmt, __VA_ARGS__)
#define WARNING(srv, fmt, ...)    _WARNING(srv, NULL, NULL, fmt, __VA_ARGS__)
#define INFO(srv, fmt, ...)       _INFO(srv, NULL, NULL, fmt, __VA_ARGS__)
#define DEBUG(srv, fmt, ...)      _DEBUG(srv, NULL, NULL, fmt, __VA_ARGS__)
#define BACKEND(srv, fmt, ...)    _BACKEND(srv, NULL, NULL, fmt, __VA_ARGS__)
#define GERROR(srv, error, fmt, ...) _GERROR(srv, NULL, NULL, error, fmt, __VA_ARGS__)

/* flags for li_log_write */
#define LI_LOG_FLAG_NONE         (0x0)      /* default flag */
#define LI_LOG_FLAG_TIMESTAMP    (0x1)      /* prepend a timestamp to the log message */

/* embed this into structures that should have their own log context, like liVRequest and liServer.logs */
struct liLogContext {
	liLogMap *log_map;
};

struct liLogTarget {
	liLogType type;
	GString *path;
	gint fd;
	liWaitQueueElem wqelem;
};

struct liLogEntry {
	GString *path;
	liLogLevel level;
	guint flags;
	GString *msg;
	GList queue_link;
};

struct liLogServerData {
	liEventLoop loop;
	liEventAsync watcher;
	liRadixTree *targets;    /** const gchar* path => (liLog*) */
	liWaitQueue close_queue;
	GQueue write_queue;
	GStaticMutex write_queue_mutex;
	GThread *thread;
	gboolean thread_alive;
	gboolean thread_finish;
	gboolean thread_stop;

	/* timestamp format cache */
	struct {
		li_tstamp last_ts;
		GString *format;
		GString *cached;
	} timestamp;

	liLogContext log_context;
};

struct liLogWorkerData {
	GQueue log_queue;
};

struct liLogMap {
	int refcount;
	GString* targets[LI_LOG_LEVEL_COUNT];
};

/* determines the type of a log target by the path given. /absolute/path = file; |app = pipe; stderr = stderr; syslog = syslog;
 * returns the begin of the parameter string in *param if param != NULL (filename for /absolute/path or file:///absolute/path)
 *   *param is either NULL or points into the path string!
 */
LI_API liLogType li_log_type_from_path(GString *path, gchar **param);

/* returns -1 for invalid names */
LI_API int li_log_level_from_string(GString *str);
LI_API gchar* li_log_level_str(liLogLevel log_level);

/* log_new is used to create a new log target, if a log with the same path already exists, it is referenced instead */
LI_API liLogTarget *li_log_new(liServer *srv, liLogType type, GString *path);

LI_API void li_log_thread_start(liServer *srv);
LI_API void li_log_thread_wakeup(liServer *srv);
LI_API void li_log_thread_stop(liServer *srv);
LI_API void li_log_thread_finish(liServer *srv);

LI_API void li_log_init(liServer *srv);
LI_API void li_log_cleanup(liServer *srv);

LI_API liLogMap* li_log_map_new(void);
LI_API liLogMap* li_log_map_new_default(void);
LI_API void li_log_map_acquire(liLogMap *log_map);
LI_API void li_log_map_release(liLogMap *log_map);

LI_API void li_log_context_set(liLogContext *context, liLogMap *log_map);

LI_API gboolean li_log_write_direct(liServer *srv, liWorker *wrk, GString *path, GString *msg);
/* li_log_write is used to write to the errorlog */
LI_API gboolean li_log_write(liServer *srv, liWorker *wrk, liLogContext* context, liLogLevel log_level, guint flags, const gchar *fmt, ...) HEDLEY_PRINTF_FORMAT(6, 7);

/* replaces '\r' and '\n' with '\0' */
LI_API void li_log_split_lines(liServer *srv, liWorker *wrk, liLogContext* context, liLogLevel log_level, guint flags, gchar *txt, const gchar *prefix);
LI_API void li_log_split_lines_(liServer *srv, liWorker *wrk, liLogContext* context, liLogLevel log_level, guint flags, gchar *txt, const gchar *fmt, ...) HEDLEY_PRINTF_FORMAT(7, 8);


#endif

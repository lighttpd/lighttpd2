#ifndef _LIGHTTPD_UTILS_H_
#define _LIGHTTPD_UTILS_H_

#include "settings.h"

LI_API void fatal(const gchar* msg);

/* set O_NONBLOCK and FD_CLOEXEC */
LI_API void fd_init(int fd);
LI_API void ev_io_add_events(struct ev_loop *loop, ev_io *watcher, int events);
LI_API void ev_io_rem_events(struct ev_loop *loop, ev_io *watcher, int events);
LI_API void ev_io_set_events(struct ev_loop *loop, ev_io *watcher, int events);


/* URL inplace decode: replace %XX with character \xXX; replace control characters with '_' (< 32 || == 127) */
LI_API void url_decode(GString *path);

LI_API void path_simplify(GString *path);

LI_API gchar *http_status_string(guint status_code);

/* */
LI_API gchar counter_format(guint64 *count, guint factor);

#endif

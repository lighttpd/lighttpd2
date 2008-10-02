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

/* returns the description for a given http status code and sets the len to the length of the returned string */
LI_API gchar *http_status_string(guint status_code, guint *len);
/* converts a given 3 digit http status code to a gchar[3] string. e.g. 403 to {'4','0','3'} */
LI_API void http_status_to_str(gint status_code, gchar status_str[]);

/* */
LI_API gchar counter_format(guint64 *count, guint factor);

LI_API gchar *ev_backend_string(guint backend);

LI_API void string_destroy_notify(gpointer str);

/* expects a pointer to a 32bit value */
LI_API guint hash_ipv4(gconstpointer key);
/* expects a pointer to a 128bit value */
LI_API guint hash_ipv6(gconstpointer key);

/* looks up the mimetype for a filename by comparing suffixes. first match is returned. do not free the result */
LI_API GString *mimetype_get(connection *con, GString *filename);

#endif

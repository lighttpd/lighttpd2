#ifndef _LIGHTTPD_UTILS_H_
#define _LIGHTTPD_UTILS_H_

#include <lighttpd/settings.h>

typedef enum {
	COUNTER_TIME,
	COUNTER_BYTES,
	COUNTER_UNITS
} liCounterType;



LI_API void fatal(const gchar* msg);

/* set O_NONBLOCK and FD_CLOEXEC */
LI_API void fd_init(int fd);
LI_API void fd_no_block(int fd);
LI_API void fd_block(int fd);

#ifndef _WIN32
/* return -2 for EAGAIN, -1 for some other error, 0 for success */
LI_API int send_fd(int s, int fd); /* write fd to unix socket s */
LI_API int receive_fd(int s, int *fd); /* read fd from unix socket s */
#endif

LI_API void ev_io_add_events(struct ev_loop *loop, ev_io *watcher, int events);
LI_API void ev_io_rem_events(struct ev_loop *loop, ev_io *watcher, int events);
LI_API void ev_io_set_events(struct ev_loop *loop, ev_io *watcher, int events);


/* URL inplace decode: replace %XX with character \xXX; replace control characters with '_' (< 32 || == 127) */
LI_API void url_decode(GString *path);

LI_API void path_simplify(GString *path);

/* formats a given guint64 for output. if dest is NULL, a new string is allocated */
LI_API GString *counter_format(guint64 count, liCounterType t, GString *dest);

LI_API gchar *ev_backend_string(guint backend);

LI_API void string_destroy_notify(gpointer str);

/* expects a pointer to a 32bit value */
LI_API guint hash_ipv4(gconstpointer key);
/* expects a pointer to a 128bit value */
LI_API guint hash_ipv6(gconstpointer key);

/* converts a sock_addr to a human readable string. ipv4 and ipv6 supported. if dest is NULL, a new string will be allocated */
LI_API GString *sockaddr_to_string(liSocketAddress addr, GString *dest, gboolean showport);

LI_API liSocketAddress sockaddr_from_string(GString *str, guint tcp_default_port);
LI_API liSocketAddress sockaddr_local_from_socket(gint fd);
LI_API liSocketAddress sockaddr_remote_from_socket(gint fd);
LI_API void sockaddr_clear(liSocketAddress *saddr);

LI_API void gstring_replace_char_with_str_len(GString *gstr, gchar c, gchar *str, guint len);

LI_API gboolean l_g_strncase_equal(GString *str, const gchar *s, guint len);

LI_API GString *l_g_string_assign_len(GString *string, const gchar *val, gssize len);

LI_API gboolean l_g_string_prefix(GString *str, const gchar *s, gsize len);
LI_API gboolean l_g_string_suffix(GString *str, const gchar *s, gsize len);

LI_API void l_g_string_append_int(GString *dest, gint64 val);

LI_API gsize dirent_buf_size(DIR * dirp);

#endif

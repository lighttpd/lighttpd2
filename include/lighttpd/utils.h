#ifndef _LIGHTTPD_UTILS_H_
#define _LIGHTTPD_UTILS_H_

#include <lighttpd/settings.h>

typedef enum {
	COUNTER_TIME,
	COUNTER_BYTES,
	COUNTER_UNITS
} liCounterType;



LI_API void li_fatal(const gchar* msg);

/* set O_NONBLOCK and FD_CLOEXEC */
LI_API void li_fd_init(int fd);
LI_API void li_fd_no_block(int fd);
LI_API void li_fd_block(int fd);

#ifndef _WIN32
/* return -2 for EAGAIN, -1 for some other error, 0 for success */
LI_API int li_send_fd(int s, int fd); /* write fd to unix socket s */
LI_API int li_receive_fd(int s, int *fd); /* read fd from unix socket s */
#endif

LI_API void li_ev_io_add_events(struct ev_loop *loop, ev_io *watcher, int events);
LI_API void li_ev_io_rem_events(struct ev_loop *loop, ev_io *watcher, int events);
LI_API void li_ev_io_set_events(struct ev_loop *loop, ev_io *watcher, int events);

#define li_ev_safe_ref_and_stop(stopf, loop, watcher) do { \
	ev_watcher *__w = (ev_watcher*) watcher;               \
	if (ev_is_active(__w)) {                               \
		ev_ref(loop);                                      \
		stopf(loop, watcher);                              \
	}                                                      \
} while (0)

/* URL inplace decode: replace %XX with character \xXX; replace control characters with '_' (< 32 || == 127) */
LI_API void li_url_decode(GString *path);

LI_API void li_path_simplify(GString *path);

/* ensures path has a trailing slash */
INLINE void li_path_append_slash(GString *path);

/* finds the first value for a given key in the querystring. works with '&' as well as ';' delimiters */
LI_API gboolean li_querystring_find(const GString *querystring, const gchar *key, const guint key_len, gchar **val, guint *val_len);

/* formats a given guint64 for output. if dest is NULL, a new string is allocated */
LI_API GString *li_counter_format(guint64 count, liCounterType t, GString *dest);

LI_API gchar *li_ev_backend_string(guint backend);

LI_API void li_string_destroy_notify(gpointer str);

LI_API guint li_hash_binary_len(gconstpointer data, gsize len);
/* expects a pointer to a 32bit value */
LI_API guint li_hash_ipv4(gconstpointer key);
/* expects a pointer to a 128bit value */
LI_API guint li_hash_ipv6(gconstpointer key);
/* expects liSocketAddress*  */
LI_API guint li_hash_sockaddr(gconstpointer key);
LI_API gboolean li_equal_sockaddr(gconstpointer key1, gconstpointer key2);

/* converts a sock_addr to a human readable string. ipv4 and ipv6 supported. if dest is NULL, a new string will be allocated */
LI_API GString *li_sockaddr_to_string(liSocketAddress addr, GString *dest, gboolean showport);

LI_API liSocketAddress li_sockaddr_from_string(const GString *str, guint tcp_default_port);
LI_API liSocketAddress li_sockaddr_local_from_socket(gint fd);
LI_API liSocketAddress li_sockaddr_remote_from_socket(gint fd);
LI_API void li_sockaddr_clear(liSocketAddress *saddr);

LI_API gboolean li_ipv4_in_ipv4_net(guint32 target, guint32 match, guint32 networkmask);
LI_API gboolean li_ipv6_in_ipv6_net(const unsigned char *target, const guint8 *match, guint network);
LI_API gboolean li_ipv6_in_ipv4_net(const unsigned char *target, guint32 match, guint32 networkmask);
LI_API gboolean li_ipv4_in_ipv6_net(guint32 target, const guint8 *match, guint network);

LI_API void li_gstring_replace_char_with_str_len(GString *gstr, gchar c, gchar *str, guint len);

INLINE GString li_const_gstring(const gchar *str, gsize len);

LI_API gboolean li_strncase_equal(const GString *str, const gchar *s, guint len);

LI_API GString *li_string_assign_len(GString *string, const gchar *val, gssize len);

LI_API gboolean li_string_prefix(const GString *str, const gchar *s, gsize len);
LI_API gboolean li_string_suffix(const GString *str, const gchar *s, gsize len);

LI_API void li_string_append_int(GString *dest, gint64 val);

LI_API gsize li_dirent_buf_size(DIR * dirp);

LI_API void li_apr_sha1_base64(GString *dest, const GString *passwd);

/* error log helper functions */
#define LI_REMOVE_PATH_FROM_FILE 1
LI_API const char *li_remove_path(const char *path);

#if LI_REMOVE_PATH_FROM_FILE
#define LI_REMOVE_PATH(file) li_remove_path(file)
#else
#define LI_REMOVE_PATH(file) file
#endif

#define LI_SYS_ERROR li_sys_error_quark()
LI_API GQuark li_sys_error_quark();

#define LI_SET_SYS_ERROR(error, msg) \
	_li_set_sys_error(error, msg, REMOVE_PATH(__FILE__), __LINE__);

LI_API gboolean _li_set_sys_error(GError **error, const gchar *msg, const gchar *file, int lineno);

#endif

/* inline implementations */

INLINE void li_path_append_slash(GString *path) {
	if (path->len == 0 || path->str[path->len-1] != '/')
		g_string_append_len(path, "/", 1);
}

/** warning: This "GString" does not make sure that there is a terminating '\0', and you shouldn't modify the GString */
INLINE GString li_const_gstring(const gchar *str, gsize len) {
	GString gs = { (gchar*) str, len, 0 };
	return gs;
}

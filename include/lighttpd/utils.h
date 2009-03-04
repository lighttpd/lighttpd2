#ifndef _LIGHTTPD_UTILS_H_
#define _LIGHTTPD_UTILS_H_

#ifndef _LIGHTTPD_BASE_H_
#error Please include <lighttpd/base.h> instead of this file
#endif

typedef enum {
	COUNTER_TIME,
	COUNTER_BYTES,
	COUNTER_BITS,
	COUNTER_UNITS
} counter_type;



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
/* returns the http method as a string and sets len to the length of the returned string */
LI_API gchar *http_method_string(http_method_t method, guint *len);
/* returns the http version as a string and sets len to the length of the returned string */
LI_API gchar *http_version_string(http_version_t method, guint *len);
/* converts a given 3 digit http status code to a gchar[3] string. e.g. 403 to {'4','0','3'} */
LI_API void http_status_to_str(gint status_code, gchar status_str[]);

/* */
LI_API gchar counter_format(guint64 *count, guint factor);
/* formats a given guint64 for output. accuracy can be a positiv integer or -1 for infinite */
LI_API GString *counter_format2(guint64 count, counter_type t, gint accuracy);

LI_API gchar *ev_backend_string(guint backend);

LI_API void string_destroy_notify(gpointer str);

/* expects a pointer to a 32bit value */
LI_API guint hash_ipv4(gconstpointer key);
/* expects a pointer to a 128bit value */
LI_API guint hash_ipv6(gconstpointer key);

/* looks up the mimetype for a filename by comparing suffixes. first match is returned. do not free the result */
LI_API GString *mimetype_get(vrequest *vr, GString *filename);

/* converts a sock_addr to a human readable string. ipv4 and ipv6 supported. if dest is NULL, a new string will be allocated */
LI_API GString *sockaddr_to_string(sock_addr *saddr, GString *dest, gboolean showport);

LI_API sockaddr sockaddr_from_string(GString *str, guint tcp_default_port);
LI_API void sockaddr_clear(sockaddr *saddr);

LI_API void gstring_replace_char_with_str_len(GString *gstr, gchar c, gchar *str, guint len);

LI_API gboolean l_g_strncase_equal(GString *str, const gchar *s, guint len);

LI_API GString *l_g_string_assign_len(GString *string, const gchar *val, gssize len);

LI_API gsize dirent_buf_size(DIR * dirp);

#endif

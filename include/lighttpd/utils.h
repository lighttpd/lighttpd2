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


struct waitqueue_elem {
	gboolean queued;
	ev_tstamp ts;
	waitqueue_elem *prev;
	waitqueue_elem *next;
	gpointer data;
};

struct waitqueue {
	waitqueue_elem *head;
	waitqueue_elem *tail;
	ev_timer timer;
	struct ev_loop *loop;
	gdouble delay;
};



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
LI_API GString *sockaddr_to_string(sock_addr *saddr, GString *dest);


/*
 * waitqueues are queues used to implement delays for certain tasks in a lightweight, non-blocking way
 * they are used for io timeouts or throttling for example
 * waitqueue_push, waitqueue_pop and waitqueue_remove have O(1) complexity
 */

/* initializes a waitqueue by creating and starting the ev_timer. precision is sub-seconds */
LI_API void waitqueue_init(waitqueue *queue, struct ev_loop *loop, waitqueue_cb callback, gdouble delay, gpointer data);
/* stops the waitqueue. to restart it, simply call waitqueue_update */
LI_API void waitqueue_stop(waitqueue *queue);
/* updates the timeout of the waitqueue, you should allways call this at the end of your callback */
LI_API void waitqueue_update(waitqueue *queue);
/* moves the element to the end of the queue if already queued, appends it to the end otherwise */
LI_API void waitqueue_push(waitqueue *queue, waitqueue_elem *elem);
/* pops the first ready! element from the queue or NULL if none ready yet. this should be called in your callback */
LI_API waitqueue_elem *waitqueue_pop(waitqueue *queue);
/* removes an element from the queue */
LI_API void waitqueue_remove(waitqueue *queue, waitqueue_elem *elem);

#endif

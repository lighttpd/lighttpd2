#ifndef _LIGHTTPD_WAITQUEUE_H_
#define _LIGHTTPD_WAITQUEUE_H_

#ifndef _LIGHTTPD_BASE_H_
#error Please include <lighttpd/base.h> instead of this file
#endif

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

/* returns the length of the queue */
LI_API guint waitqueue_length(waitqueue *queue);

#endif

#ifndef _LIGHTTPD_WAITQUEUE_H_
#define _LIGHTTPD_WAITQUEUE_H_

#include <lighttpd/settings.h>

typedef struct liWaitQueueElem liWaitQueueElem;
typedef struct liWaitQueue liWaitQueue;
typedef void (*liWaitQueueCB) (struct ev_loop *loop, struct ev_timer *w, int revents);

struct liWaitQueueElem {
	gboolean queued;
	ev_tstamp ts;
	liWaitQueueElem *prev;
	liWaitQueueElem *next;
	gpointer data;
};

struct liWaitQueue {
	liWaitQueueElem *head;
	liWaitQueueElem *tail;
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
LI_API void waitqueue_init(liWaitQueue *queue, struct ev_loop *loop, liWaitQueueCB callback, gdouble delay, gpointer data);

/* stops the waitqueue. to restart it, simply call waitqueue_update */
LI_API void waitqueue_stop(liWaitQueue *queue);

/* updates the timeout of the waitqueue, you should allways call this at the end of your callback */
LI_API void waitqueue_update(liWaitQueue *queue);

/* moves the element to the end of the queue if already queued, appends it to the end otherwise */
LI_API void waitqueue_push(liWaitQueue *queue, liWaitQueueElem *elem);

/* pops the first ready! element from the queue or NULL if none ready yet. this should be called in your callback */
LI_API liWaitQueueElem *waitqueue_pop(liWaitQueue *queue);

/* pops all elements from the queue that are ready or NULL of none ready yet. returns number of elements pop()ed and saves old head in '*head' */
LI_API guint waitqueue_pop_ready(liWaitQueue *queue, liWaitQueueElem **head);

/* removes an element from the queue */
LI_API void waitqueue_remove(liWaitQueue *queue, liWaitQueueElem *elem);

/* returns the length of the queue */
LI_API guint waitqueue_length(liWaitQueue *queue);

#endif

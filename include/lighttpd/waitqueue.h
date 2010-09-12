#ifndef _LIGHTTPD_WAITQUEUE_H_
#define _LIGHTTPD_WAITQUEUE_H_

#include <lighttpd/settings.h>

typedef struct liWaitQueueElem liWaitQueueElem;
typedef struct liWaitQueue liWaitQueue;
typedef void (*liWaitQueueCB) (liWaitQueue *wq, gpointer data);

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

	liWaitQueueCB callback;
	gpointer data;
	guint length;
};

/*
 * waitqueues are queues used to implement delays for certain tasks in a lightweight, non-blocking way
 * they are used for io timeouts or throttling for example
 * li_waitqueue_push, li_waitqueue_pop and li_waitqueue_remove have O(1) complexity
 */

/* initializes a waitqueue by creating the ev_timer and initializing the queue. precision is sub-seconds */
LI_API void li_waitqueue_init(liWaitQueue *queue, struct ev_loop *loop, liWaitQueueCB callback, gdouble delay, gpointer data);

/* stops the waitqueue. to restart it, simply call li_waitqueue_update */
LI_API void li_waitqueue_stop(liWaitQueue *queue);

/* updates the delay of the timer. if timer is active, it is stopped and restarted */
LI_API void li_waitqueue_set_delay(liWaitQueue *queue, gdouble delay);

/* updates the timeout of the waitqueue, you should allways call this at the end of your callback */
LI_API void li_waitqueue_update(liWaitQueue *queue);

/* moves the element to the end of the queue if already queued, appends it to the end otherwise */
LI_API void li_waitqueue_push(liWaitQueue *queue, liWaitQueueElem *elem);

/* pops the first ready! element from the queue or NULL if none ready yet. this should be called in your callback */
LI_API liWaitQueueElem *li_waitqueue_pop(liWaitQueue *queue);

/* pops the first element from the queue or NULL if empty. use it to clean your queue */
LI_API liWaitQueueElem *li_waitqueue_pop_force(liWaitQueue *queue);

/* pops all elements from the queue that are ready or NULL of none ready yet. returns number of elements pop()ed and saves old head in '*head' */
LI_API guint li_waitqueue_pop_ready(liWaitQueue *queue, liWaitQueueElem **head);

/* removes an element from the queue */
LI_API void li_waitqueue_remove(liWaitQueue *queue, liWaitQueueElem *elem);

#endif

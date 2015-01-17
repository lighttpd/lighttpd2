
#include <lighttpd/waitqueue.h>

static void wq_cb(liEventBase *watcher, int events) {
	liWaitQueue *queue = LI_CONTAINER_OF(li_event_timer_from(watcher), liWaitQueue, timer);
	UNUSED(events);

	queue->callback(queue, queue->data);
}

void li_waitqueue_init(liWaitQueue *queue, liEventLoop *loop, liWaitQueueCB callback, gdouble delay, gpointer data) {
	li_event_timer_init(loop, "waitqueue", &queue->timer, wq_cb);

	queue->head = queue->tail = NULL;
	queue->delay = delay;
	queue->callback = callback;
	queue->data = data;
	queue->length = 0;
}

void li_waitqueue_stop(liWaitQueue *queue) {
	li_event_clear(&queue->timer);
}

void li_waitqueue_set_delay(liWaitQueue *queue, gdouble delay) {
	queue->delay = delay;

	if (li_event_active(&queue->timer)) {
		li_waitqueue_update(queue);
	}
}

void li_waitqueue_update(liWaitQueue *queue) {
	li_tstamp repeat;
	li_tstamp now = li_event_now(li_event_get_loop(&queue->timer));

	if (G_LIKELY(queue->head)) {
		repeat = queue->head->ts + queue->delay - now;

		if (repeat < 0.05) repeat = 0.05;

		li_event_timer_once(&queue->timer, repeat);
	} else {
		/* stop timer if queue empty */
		li_event_stop(&queue->timer);
		return;
	}
}

void li_waitqueue_push(liWaitQueue *queue, liWaitQueueElem *elem) {
	elem->ts = li_event_now(li_event_get_loop(&queue->timer));

	if (!elem->queued) {
		/* not in the queue yet, insert at the end */
		elem->queued = TRUE;
		queue->length++;

		if (!queue->head) {
			/* queue is empty */
			queue->head = elem;
			queue->tail = elem;
			elem->prev = NULL;
			elem->next = NULL;
		} else {
			/* queue not empty */
			elem->prev = queue->tail;
			elem->next = NULL;
			queue->tail->next = elem;
			queue->tail = elem;
		}
	} else {
		/* already queued, move to end */
		if (elem == queue->tail)
			return;

		if (elem == queue->head)
			queue->head = elem->next;
		else
			elem->prev->next = elem->next;

		elem->next->prev = elem->prev;
		elem->prev = queue->tail;
		elem->next = NULL;
		queue->tail->next = elem;
		queue->tail = elem;
	}

	if (G_UNLIKELY(!li_event_active(&queue->timer)))
		li_event_timer_once(&queue->timer, queue->delay);
}

liWaitQueueElem *li_waitqueue_pop(liWaitQueue *queue) {
	liWaitQueueElem *elem = queue->head;
	li_tstamp now = li_event_now(li_event_get_loop(&queue->timer));

	if (!elem || (elem->ts + queue->delay) > now) {
		return NULL;
	}

	if (elem == queue->tail)
		queue->tail = NULL;
	else
		elem->next->prev = NULL;

	queue->head = elem->next;
	queue->length--;

	elem->ts = 0;
	elem->queued = FALSE;

	return elem;
}

liWaitQueueElem *li_waitqueue_pop_force(liWaitQueue *queue) {
	liWaitQueueElem *elem = queue->head;

	if (!elem) {
		return NULL;
	}

	if (elem == queue->tail)
		queue->tail = NULL;
	else
		elem->next->prev = NULL;

	queue->head = elem->next;
	queue->length--;

	elem->ts = 0;
	elem->queued = FALSE;

	return elem;
}

void li_waitqueue_remove(liWaitQueue *queue, liWaitQueueElem *elem) {
	if (!elem->queued)
		return;

	if (elem == queue->head)
		queue->head = elem->next;
	else
		elem->prev->next = elem->next;

	if (elem == queue->tail)
		queue->tail = elem->prev;
	else
		elem->next->prev = elem->prev;

	elem->ts = 0;
	elem->queued = FALSE;
	queue->length--;

	if (G_UNLIKELY(!queue->head))
		li_event_stop(&queue->timer);
}

guint li_waitqueue_pop_ready(liWaitQueue *queue, liWaitQueueElem **head) {
	guint i = 0;
	liWaitQueueElem *elem = queue->head;
	li_tstamp now = li_event_now(li_event_get_loop(&queue->timer));

	*head = elem;

	while (elem != NULL) {
		if ((elem->ts + queue->delay) > now) {
			queue->head = elem;

			if (elem->prev) {
				elem->prev->next = NULL;
			}

			return i;
		}

		elem->ts = 0;
		elem->queued = FALSE;
		elem = elem->next;
		queue->length--;
		i++;
	}

	queue->head = NULL;
	queue->tail = NULL;

	return i;
}

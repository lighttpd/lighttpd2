
#include <lighttpd/waitqueue.h>

void li_waitqueue_init(liWaitQueue *queue, struct ev_loop *loop, liWaitQueueCB callback, gdouble delay, gpointer data) {
	ev_timer_init(&queue->timer, callback, delay, delay);

	queue->timer.data = data;
	queue->head = queue->tail = NULL;
	queue->loop = loop;
	queue->delay = delay;
}

void li_waitqueue_stop(liWaitQueue *queue) {
	ev_timer_stop(queue->loop, &queue->timer);
}

void li_waitqueue_update(liWaitQueue *queue) {
	ev_tstamp repeat;
	ev_tstamp now = ev_now(queue->loop);

	if (G_LIKELY(queue->head)) {
		repeat = queue->head->ts + queue->delay - now;

		if (repeat < 0.05)
			repeat = 0.05;

		queue->timer.repeat = repeat;
		ev_timer_again(queue->loop, &queue->timer);
	} else {
		/* stop timer if queue empty */
		ev_timer_stop(queue->loop, &queue->timer);
		return;
	}
}

void li_waitqueue_push(liWaitQueue *queue, liWaitQueueElem *elem) {
	elem->ts = ev_now(queue->loop);

	if (!elem->queued) {
		/* not in the queue yet, insert at the end */
		elem->queued = TRUE;
		
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

	if (G_UNLIKELY(!ev_is_active(&queue->timer)))
		ev_timer_start(queue->loop, &queue->timer);
}

liWaitQueueElem *li_waitqueue_pop(liWaitQueue *queue) {
	liWaitQueueElem *elem = queue->head;
	ev_tstamp now = ev_now(queue->loop);

	if (!elem || (elem->ts + queue->delay) > now) {
		return NULL;
	}

	if (elem == queue->tail)
		queue->tail = NULL;
	else
		elem->next->prev = NULL;

	queue->head = elem->next;

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

	if (G_UNLIKELY(!queue->head))
		ev_timer_stop(queue->loop, &queue->timer);
}


guint li_waitqueue_length(liWaitQueue *queue) {
	guint i = 0;
	liWaitQueueElem *elem = queue->head;

	while (elem) {
		i++;
		elem = elem->next;
	}

	return i;
}

guint li_waitqueue_pop_ready(liWaitQueue *queue, liWaitQueueElem **head) {
	guint i = 0;
	liWaitQueueElem *elem = queue->head;
	ev_tstamp now = ev_now(queue->loop);

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
		i++;
	}

	queue->head = NULL;
	queue->tail = NULL;

	return i;
}

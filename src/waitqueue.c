
#include <lighttpd/base.h>

void waitqueue_init(waitqueue *queue, struct ev_loop *loop, waitqueue_cb callback, gdouble delay, gpointer data) {
	ev_timer_init(&queue->timer, callback, delay, delay);
	ev_timer_start(loop, &queue->timer);

	queue->timer.data = data;
	queue->head = queue->tail = NULL;
	queue->loop = loop;
	queue->delay = delay;
}

void waitqueue_stop(waitqueue *queue) {
	ev_timer_stop(queue->loop, &queue->timer);
}

void waitqueue_update(waitqueue *queue) {
	ev_tstamp repeat;

	if (queue->head) {
		repeat = queue->head->ts + queue->delay - ev_now(queue->loop);
		if (repeat < 0.01)
			repeat = 0.01;
	} else {
		repeat = queue->delay;
	}

	if (queue->timer.repeat != repeat)
	{
		queue->timer.repeat = repeat;
		ev_timer_again(queue->loop, &queue->timer);
	}
}

void waitqueue_push(waitqueue *queue, waitqueue_elem *elem) {
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
}

waitqueue_elem *waitqueue_pop(waitqueue *queue) {
	waitqueue_elem *elem = queue->head;
	ev_tstamp now = ev_now(queue->loop);

	if (!elem || (elem->ts + queue->delay) > now) {
		return NULL;
	}

	if (elem == queue->tail)
		queue->tail = NULL;
	else
		elem->next->prev = NULL;

	queue->head = elem->next;

	elem->queued = FALSE;

	return elem;
}

void waitqueue_remove(waitqueue *queue, waitqueue_elem *elem) {
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

	elem->queued = FALSE;
	elem->ts = 0;
}


guint waitqueue_length(waitqueue *queue) {
	guint i = 0;
	waitqueue_elem *elem = queue->head;

	while (elem) {
		i++;
		elem = elem->next;
	}

	return i;
}

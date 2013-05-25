
#include <lighttpd/events.h>

/* closing sockets - wait for proper shutdown */

typedef struct closing_socket closing_socket;

struct closing_socket {
	liEventLoop *loop;
	GList sockets_link;
	int fd;
	li_tstamp close_timeout;
};

static void close_socket_now(closing_socket *cs) {
	close(cs->fd);
	cs->fd = -1;
	g_queue_unlink(&cs->loop->closing_sockets, &cs->sockets_link);
}

static void closing_socket_cb(int revents, void* arg) {
	static char trash[1024];
	closing_socket *cs = (closing_socket*) arg;
	ssize_t r;
	liEventLoop *loop = cs->loop;
	li_tstamp remaining = cs->close_timeout - li_event_now(loop);

	if (-1 == cs->fd) {
		g_slice_free(closing_socket, cs);
		return;
	}

	/* empty the input buffer, wait for EOF or timeout or a socket error to close it */
	for (;!loop->end;) {
		r = read(cs->fd, trash, sizeof(trash));
		if (0 == r) break; /* got EOF */
		if (0 > r) { /* error */
			switch (errno) {
			case EINTR:
				/* call read again */
				continue;
			case EAGAIN:
#if EWOULDBLOCK != EAGAIN
			case EWOULDBLOCK:
#endif
				/* check timeout: */
				if (remaining > 0 && !(revents & EV_TIMEOUT)) {
					/* wait again */
					ev_once(cs->loop->loop, cs->fd, EV_READ, remaining, closing_socket_cb, cs);
					return;
				}
				/* timeout reached, break switch and loop */
				break;
			default:
				/* real error (probably ECONNRESET or similar): break switch and loop */
				/* no logging: there is no context anymore for the socket */
				break;
			}
			break; /* end loop */
		}
	}

	close_socket_now(cs);
	g_slice_free(closing_socket, cs);
}

void li_event_add_closing_socket(liEventLoop *loop, int fd) {
	closing_socket *cs;

	if (-1 == fd) return;

	shutdown(fd, SHUT_WR);
	if (loop->end) {
		close(fd);
		return;
	}

	cs = g_slice_new0(closing_socket);
	cs->loop = loop;
	cs->fd = fd;
	g_queue_push_tail_link(&loop->closing_sockets, &cs->sockets_link);
	cs->close_timeout = li_event_now(loop) + 10.0;

	ev_once(loop->loop, fd, EV_READ, 10.0, closing_socket_cb, cs);
}


void li_event_loop_init(liEventLoop *loop, struct ev_loop *evloop) {
	ev_ref(evloop);
	loop->end = 0;
	loop->loop = evloop;
	g_queue_init(&loop->watchers);
	g_queue_init(&loop->closing_sockets);
	li_job_queue_init(&loop->jobqueue, loop);
}

struct ev_loop* li_event_loop_clear(liEventLoop *loop) {
	struct ev_loop* evloop = loop->loop;
	GList *lnk;

	li_event_loop_end(loop);
	li_job_queue_clear(&loop->jobqueue);

	while (NULL != (lnk = loop->watchers.head)) {
		liEventBase *base = LI_CONTAINER_OF(lnk, liEventBase, link_watchers);
		assert(li_event_attached_(base));
		li_event_detach_(base);
		assert(lnk != loop->watchers.head);
	}
	loop->loop = NULL;
	return evloop;
}

void li_event_loop_run(liEventLoop *loop) {
	ev_loop(loop->loop, 0);
}

void li_event_loop_end(liEventLoop *loop) {
	if (loop->end) return;
	loop->end = TRUE;
	ev_unref(loop->loop);

	li_event_loop_force_close_sockets(loop);
}

void li_event_loop_exit(liEventLoop *loop) {
	li_event_loop_end(loop);
	ev_unloop(loop->loop, EVUNLOOP_ALL);
}

void li_event_loop_force_close_sockets(liEventLoop *loop) {
	GList *lnk;

	while (NULL != (lnk = loop->closing_sockets.head)) {
		closing_socket *cs = LI_CONTAINER_OF(lnk, closing_socket, sockets_link);
		ev_feed_fd_event(loop->loop, cs->fd, EV_READ);
		close_socket_now(cs);
	}
}

const char* li_event_loop_backend_string(liEventLoop *loop) {
	switch (ev_backend(loop->loop)) {
	case EVBACKEND_SELECT:  return "select";
	case EVBACKEND_POLL:    return "poll";
	case EVBACKEND_EPOLL:   return "epoll";
	case EVBACKEND_KQUEUE:  return "kqueue";
	case EVBACKEND_DEVPOLL: return "devpoll";
	case EVBACKEND_PORT:    return "port";
	default:                return "unknown";
	}
}

static void event_io_cb(struct ev_loop *loop, ev_io *w, int revents) {
	liEventIO *io = LI_CONTAINER_OF(w, liEventIO, libevmess.io);
	liEventLoop *my_loop = io->base.link_watchers.data;
	int events = 0;

	assert(NULL != my_loop);
	assert(loop == my_loop->loop);

	if (revents & EV_READ) events |= LI_EV_READ;
	if (revents & EV_WRITE) events |= LI_EV_WRITE;

	io->base.callback(&io->base, events);
}

static int io_events_to_libev(int events) {
	int revents = 0;
	if (events & LI_EV_READ) revents |= EV_READ;
	if (events & LI_EV_WRITE) revents |= EV_WRITE;
	return revents;
}

void li_event_io_init(liEventLoop *loop, liEventIO *io, liEventCallback callback, int fd, int events) {
	memset(io, 0, sizeof(io));
	io->base.type = LI_EVT_IO;
	io->base.keep_loop_alive = 1;
	io->base.callback = callback;
	io->events = events;
	ev_init(&io->libevmess.w, NULL);
	ev_io_set(&io->libevmess.io, fd, io_events_to_libev(events));
	ev_set_cb(&io->libevmess.io, event_io_cb);

	if (NULL != loop) li_event_attach(loop, io);
}

void li_event_io_set_fd(liEventIO *io, int fd) {
	if (-1 == fd) {
		li_event_stop(io);
		ev_io_set(&io->libevmess.io, fd, io->libevmess.io.events);
		return;
	}

	if (li_event_attached(io) && li_event_active(io)) {
		liEventLoop *loop = io->base.link_watchers.data;
		assert(NULL != loop);

		ev_ref(loop->loop);

		ev_io_stop(loop->loop, &io->libevmess.io);
		ev_io_set(&io->libevmess.io, fd, io->libevmess.io.events);
		ev_io_start(loop->loop, &io->libevmess.io);

		ev_unref(loop->loop);
	} else {
		ev_io_set(&io->libevmess.io, fd, io->libevmess.io.events);
	}
}

void li_event_io_set_events(liEventIO *io, int events) {
	if (events == io->events) return;
	io->events = events;

	if (li_event_attached(io) && li_event_active(io)) {
		liEventLoop *loop = io->base.link_watchers.data;
		assert(NULL != loop);

		ev_ref(loop->loop);

		ev_io_stop(loop->loop, &io->libevmess.io);
		ev_io_set(&io->libevmess.io, io->libevmess.io.fd, io_events_to_libev(events));
		ev_io_start(loop->loop, &io->libevmess.io);

		ev_unref(loop->loop);
	} else {
		ev_io_set(&io->libevmess.io, io->libevmess.io.fd, io_events_to_libev(events));
	}
}

void li_event_io_add_events(liEventIO *io, int events) {
	li_event_io_set_events(io, io->events | events);
}
void li_event_io_rem_events(liEventIO *io, int events) {
	li_event_io_set_events(io, io->events & ~events);
}

static void event_timer_cb(struct ev_loop *loop, ev_timer *w, int revents) {
	liEventTimer *timer = LI_CONTAINER_OF(w, liEventTimer, libevmess.timer);
	liEventLoop *my_loop = timer->base.link_watchers.data;
	UNUSED(revents);

	assert(NULL != my_loop);
	assert(loop == my_loop->loop);

	if (ev_is_active(w)) {
		if (!timer->base.keep_loop_alive) ev_ref(loop);
		ev_timer_stop(loop, w);
	}
	timer->base.active = 0;

	timer->base.callback(&timer->base, LI_EV_WAKEUP);
}

void li_event_timer_init(liEventLoop *loop, liEventTimer *timer, liEventCallback callback) {
	memset(timer, 0, sizeof(timer));
	timer->base.type = LI_EVT_TIMER;
	timer->base.keep_loop_alive = 1;
	timer->base.callback = callback;
	ev_init(&timer->libevmess.w, NULL);
	ev_set_cb(&timer->libevmess.timer, event_timer_cb);

	if (NULL != loop) li_event_attach(loop, timer);
}

static void event_async_cb(struct ev_loop *loop, ev_async *w, int revents) {
	liEventAsync *async = LI_CONTAINER_OF(w, liEventAsync, libevmess.async);
	liEventLoop *my_loop = async->base.link_watchers.data;
	UNUSED(revents);

	assert(NULL != my_loop);
	assert(loop == my_loop->loop);

	async->base.callback(&async->base, LI_EV_WAKEUP);
}

void li_event_async_init(liEventLoop *loop, liEventAsync *async, liEventCallback callback) {
	memset(async, 0, sizeof(async));
	async->base.type = LI_EVT_ASYNC;
	async->base.keep_loop_alive = 0;
	async->base.callback = callback;
	ev_init(&async->libevmess.w, NULL);
	ev_set_cb(&async->libevmess.async, event_async_cb);

	if (NULL != loop) li_event_attach(loop, async);
	li_event_start(async);
}

static void event_child_cb(struct ev_loop *loop, ev_child *w, int revents) {
	liEventChild *child = LI_CONTAINER_OF(w, liEventChild, libevmess.child);
	liEventLoop *my_loop = child->base.link_watchers.data;
	UNUSED(revents);

	assert(NULL != my_loop);
	assert(loop == my_loop->loop);

	if (ev_is_active(w)) {
		if (!child->base.keep_loop_alive) ev_ref(loop);
		ev_child_stop(loop, w);
	}
	child->base.active = 0;

	child->base.callback(&child->base, LI_EV_WAKEUP);
}

void li_event_child_init(liEventLoop *loop, liEventChild *child, liEventCallback callback, int pid) {
	memset(child, 0, sizeof(child));
	child->base.type = LI_EVT_CHILD;
	child->base.keep_loop_alive = 1;
	child->base.callback = callback;
	ev_init(&child->libevmess.w, NULL);
	ev_child_set(&child->libevmess.child, pid, 0);
	ev_set_cb(&child->libevmess.child, event_child_cb);

	if (NULL != loop) li_event_attach(loop, child);
	li_event_start(child);
}

static void event_signal_cb(struct ev_loop *loop, ev_signal *w, int revents) {
	liEventSignal *sig = LI_CONTAINER_OF(w, liEventSignal, libevmess.sig);
	liEventLoop *my_loop = sig->base.link_watchers.data;
	UNUSED(revents);

	assert(NULL != my_loop);
	assert(loop == my_loop->loop);

	sig->base.callback(&sig->base, LI_EV_WAKEUP);
}

void li_event_signal_init(liEventLoop *loop, liEventSignal *sig, liEventCallback callback, int signum) {
	memset(sig, 0, sizeof(sig));
	sig->base.type = LI_EVT_SIGNAL;
	sig->base.keep_loop_alive = 0;
	sig->base.callback = callback;
	ev_init(&sig->libevmess.w, NULL);
	ev_signal_set(&sig->libevmess.sig, signum);
	ev_set_cb(&sig->libevmess.sig, event_signal_cb);

	if (NULL != loop) li_event_attach(loop, sig);
	li_event_start(sig);
}

static void event_prepare_cb(struct ev_loop *loop, ev_prepare *w, int revents) {
	liEventPrepare *prepare = LI_CONTAINER_OF(w, liEventPrepare, libevmess.prepare);
	liEventLoop *my_loop = prepare->base.link_watchers.data;
	UNUSED(revents);

	assert(NULL != my_loop);
	assert(loop == my_loop->loop);

	prepare->base.callback(&prepare->base, LI_EV_WAKEUP);
}

void li_event_prepare_init(liEventLoop *loop, liEventPrepare *prepare, liEventCallback callback) {
	memset(prepare, 0, sizeof(prepare));
	prepare->base.type = LI_EVT_PREPARE;
	prepare->base.keep_loop_alive = 0;
	prepare->base.callback = callback;
	ev_init(&prepare->libevmess.w, NULL);
	ev_set_cb(&prepare->libevmess.prepare, event_prepare_cb);

	if (NULL != loop) li_event_attach(loop, prepare);
	li_event_start(prepare);
}

static void event_check_cb(struct ev_loop *loop, ev_check *w, int revents) {
	liEventCheck *check = LI_CONTAINER_OF(w, liEventCheck, libevmess.check);
	liEventLoop *my_loop = check->base.link_watchers.data;
	UNUSED(revents);

	assert(NULL != my_loop);
	assert(loop == my_loop->loop);

	check->base.callback(&check->base, LI_EV_WAKEUP);
}

void li_event_check_init(liEventLoop *loop, liEventCheck *check, liEventCallback callback) {
	memset(check, 0, sizeof(check));
	check->base.type = LI_EVT_CHECK;
	check->base.keep_loop_alive = 0;
	check->base.callback = callback;
	ev_init(&check->libevmess.w, NULL);
	ev_set_cb(&check->libevmess.check, event_check_cb);

	if (NULL != loop) li_event_attach(loop, check);
	li_event_start(check);
}

#ifndef _LIGHTTPD_EVENTS_H_
#define _LIGHTTPD_EVENTS_H_

#include <lighttpd/settings.h>
#include <lighttpd/utils.h>

enum {
	LI_EV_READ    = 0x01,
	LI_EV_WRITE   = 0x02,
	LI_EV_WAKEUP  = 0x04
};

typedef enum {
	LI_EVT_NONE = 0,
	LI_EVT_IO,
	LI_EVT_TIMER,
	LI_EVT_ASYNC,
	LI_EVT_CHILD,
	LI_EVT_SIGNAL,
	LI_EVT_PREPARE,
	LI_EVT_CHECK
} liEventType;

typedef struct liEventLoop liEventLoop;
typedef struct liEventBase liEventBase;
typedef struct liEventIO liEventIO;
typedef struct liEventTimer liEventTimer;
typedef struct liEventAsync liEventAsync;
typedef struct liEventChild liEventChild;
typedef struct liEventSignal liEventSignal;
typedef struct liEventPrepare liEventPrepare;
typedef struct liEventCheck liEventCheck;

typedef ev_tstamp li_tstamp;

typedef void (*liEventCallback)(liEventBase *watcher, int events);

struct liEventBase {
	liEventType type;
	unsigned int keep_loop_alive:1, active: 1;
	GList link_watchers; /* data points to loop */
	const char *event_name; /* track what the event is used for */
	liEventCallback callback;
};

struct liEventIO {
	liEventBase base;
	int events;
	union {
		struct ev_watcher w;
		/* struct ev_watcher_list l; */
		struct ev_io io;
	} libevmess;
};

struct liEventTimer {
	liEventBase base;
	union {
		struct ev_watcher w;
		struct ev_timer timer;
	} libevmess;
};

struct liEventAsync {
	liEventBase base;
	union {
		struct ev_watcher w;
		struct ev_async async;
	} libevmess;
};

struct liEventChild {
	liEventBase base;
	union {
		struct ev_watcher w;
		struct ev_child child;
	} libevmess;
};

struct liEventSignal {
	liEventBase base;
	union {
		struct ev_watcher w;
		struct ev_signal sig;
	} libevmess;
};

struct liEventPrepare {
	liEventBase base;
	union {
		struct ev_watcher w;
		struct ev_prepare prepare;
	} libevmess;
};

struct liEventCheck {
	liEventBase base;
	union {
		struct ev_watcher w;
		struct ev_check check;
	} libevmess;
};

#include <lighttpd/jobqueue.h>

struct liEventLoop {
	struct ev_loop *loop;
	liJobQueue jobqueue;
	GQueue watchers;
	GQueue closing_sockets;
	/* whether loop should exit once all "keep_loop_alive" watchers are dead */
	unsigned int end:1;
};

LI_API void li_event_loop_init(liEventLoop *loop, struct ev_loop *evloop);
LI_API struct ev_loop* li_event_loop_clear(liEventLoop *loop);
LI_API void li_event_loop_run(liEventLoop *loop);
LI_API void li_event_loop_end(liEventLoop *loop);
LI_API void li_event_loop_exit(liEventLoop *loop);
LI_API void li_event_loop_force_close_sockets(liEventLoop *loop);

LI_API const char* li_event_loop_backend_string(liEventLoop *loop);

INLINE li_tstamp li_event_time(void);
INLINE li_tstamp li_event_now(liEventLoop *loop);

LI_API void li_event_add_closing_socket(liEventLoop *loop, int fd);

INLINE void li_event_attach_(liEventLoop *loop, liEventBase *base);
INLINE void li_event_detach_(liEventBase *base);
INLINE gboolean li_event_attached_(liEventBase *base);
INLINE liEventLoop* li_event_get_loop_(liEventBase *base);

INLINE void li_event_start_(liEventBase *base);
INLINE void li_event_stop_(liEventBase *base);
INLINE gboolean li_event_active_(liEventBase *base);

INLINE void li_event_set_keep_loop_alive_(liEventBase *base, gboolean keep_loop_alive);

INLINE void li_event_clear_(liEventBase *base);
INLINE void li_event_set_callback_(liEventBase *base, liEventCallback callback);

#define li_event_attach(loop, watcher) (li_event_attach_((loop), &(watcher)->base))
#define li_event_detach(watcher) (li_event_detach_(&(watcher)->base))
#define li_event_attached(watcher) (li_event_attached_(&(watcher)->base))
#define li_event_get_loop(watcher) (li_event_get_loop_(&(watcher)->base))
#define li_event_start(watcher) (li_event_start_(&(watcher)->base))
#define li_event_stop(watcher) (li_event_stop_(&(watcher)->base))
#define li_event_active(watcher) (li_event_active_(&(watcher)->base))
#define li_event_set_keep_loop_alive(watcher, keep_loop_alive) (li_event_set_keep_loop_alive_(&(watcher)->base, keep_loop_alive))
#define li_event_clear(watcher) (li_event_clear_(&(watcher)->base))
#define li_event_set_callback(watcher, callback) (li_event_set_callback_(&(watcher)->base, callback))

/* defaults to keep_loop_alive = TRUE */
LI_API void li_event_io_init(liEventLoop *loop, const char *event_name, liEventIO *io, liEventCallback callback, int fd, int events);
LI_API void li_event_io_set_fd(liEventIO *io, int fd);
INLINE int li_event_io_fd(liEventIO *io);
LI_API void li_event_io_set_events(liEventIO *io, int events);
LI_API void li_event_io_add_events(liEventIO *io, int events);
LI_API void li_event_io_rem_events(liEventIO *io, int events);
INLINE liEventIO* li_event_io_from(liEventBase *base);

/* defaults to keep_loop_alive = TRUE */
/* timer will always stop when it triggers */
LI_API void li_event_timer_init(liEventLoop *loop, const char *event_name, liEventTimer *timer, liEventCallback callback);
INLINE void li_event_timer_once(liEventTimer *timer, li_tstamp timeout); /* also starts the watcher */
INLINE liEventTimer* li_event_timer_from(liEventBase *base);

/* defaults to keep_loop_alive = FALSE, starts immediately */
LI_API void li_event_async_init(liEventLoop *loop, const char *event_name, liEventAsync *async, liEventCallback callback);
INLINE void li_event_async_send(liEventAsync *async);
INLINE liEventAsync* li_event_async_from(liEventBase *base);

/* defaults to keep_loop_alive = TRUE, starts immediately */
LI_API void li_event_child_init(liEventLoop *loop, const char *event_name, liEventChild *child, liEventCallback callback, int pid);
INLINE int li_event_child_pid(liEventChild *child);
INLINE int li_event_child_status(liEventChild *child);
INLINE liEventChild* li_event_child_from(liEventBase *base);

/* defaults to keep_loop_alive = FALSE, starts immediately */
LI_API void li_event_signal_init(liEventLoop *loop, const char *event_name, liEventSignal *signal, liEventCallback callback, int signum);
INLINE int li_event_signal_signum(liEventSignal *signal);
INLINE liEventSignal* li_event_signal_from(liEventBase *base);

/* defaults to keep_loop_alive = FALSE, starts immediately */
LI_API void li_event_prepare_init(liEventLoop *loop, const char *event_name, liEventPrepare *prepare, liEventCallback callback);
INLINE liEventPrepare* li_event_prepare_from(liEventBase *base);

/* defaults to keep_loop_alive = FALSE, starts immediately */
LI_API void li_event_check_init(liEventLoop *loop, const char *event_name, liEventCheck *check, liEventCallback callback);
INLINE liEventCheck* li_event_check_from(liEventBase *base);

LI_API const char* li_event_type_string(liEventType type);

/* inline implementations */

#pragma GCC diagnostic ignored "-Wunknown-pragmas"
#pragma clang diagnostic push
#pragma GCC diagnostic ignored "-Wstrict-aliasing"

INLINE li_tstamp li_event_now(liEventLoop *loop) {
	return ev_now(loop->loop);
}

INLINE li_tstamp li_event_time(void) {
	return ev_time();
}

INLINE void li_event_attach_(liEventLoop *loop, liEventBase *base) {
	LI_FORCE_ASSERT(NULL == base->link_watchers.data);
	LI_FORCE_ASSERT(NULL != loop);

	base->link_watchers.data = loop;
	g_queue_push_tail_link(&loop->watchers, &base->link_watchers);

	if (base->active) {
		base->active = 0;
		li_event_start_(base);
	}
}

INLINE void li_event_detach_(liEventBase *base) {
	liEventLoop *loop = base->link_watchers.data;

	if (NULL == loop) return;

	if (base->active) {
		li_event_stop_(base);
		base->active = 1;
	}

	base->link_watchers.data = NULL;
	g_queue_unlink(&loop->watchers, &base->link_watchers);
}

INLINE gboolean li_event_attached_(liEventBase *base) {
	return NULL != base->link_watchers.data;
}

INLINE liEventLoop* li_event_get_loop_(liEventBase *base) {
	return base->link_watchers.data;
}

INLINE void li_event_start_(liEventBase *base) {
	liEventLoop *loop = base->link_watchers.data;

	LI_FORCE_ASSERT(NULL != base->callback);
	LI_FORCE_ASSERT(LI_EVT_NONE != base->type);

	if (base->active) return;
	base->active = 1;

	if (NULL != loop) {
		switch (base->type) {
		case LI_EVT_NONE:
			break;
		case LI_EVT_IO:
			{
				liEventIO *io = li_event_io_from(base);
				LI_FORCE_ASSERT(!ev_is_active(&io->libevmess.w));
				LI_FORCE_ASSERT(-1 != io->libevmess.io.fd);
				ev_io_start(loop->loop, &io->libevmess.io);
				if (!base->keep_loop_alive) ev_unref(loop->loop);
			}
			break;
		case LI_EVT_TIMER:
			{
				liEventTimer *timer = li_event_timer_from(base);
				LI_FORCE_ASSERT(!ev_is_active(&timer->libevmess.w));
				if (0 >= timer->libevmess.timer.repeat) timer->libevmess.timer.repeat = 0.0001;
				ev_timer_again(loop->loop, &timer->libevmess.timer);
				if (!base->keep_loop_alive) ev_unref(loop->loop);
			}
			break;
		case LI_EVT_ASYNC:
			{
				liEventAsync *async = li_event_async_from(base);
				LI_FORCE_ASSERT(!ev_is_active(&async->libevmess.w));
				ev_async_start(loop->loop, &async->libevmess.async);
				if (!base->keep_loop_alive) ev_unref(loop->loop);
			}
			break;
		case LI_EVT_CHILD:
			{
				liEventChild *child = li_event_child_from(base);
				LI_FORCE_ASSERT(!ev_is_active(&child->libevmess.w));
				ev_child_start(loop->loop, &child->libevmess.child);
				if (!base->keep_loop_alive) ev_unref(loop->loop);
			}
			break;
		case LI_EVT_SIGNAL:
			{
				liEventSignal *sig = li_event_signal_from(base);
				LI_FORCE_ASSERT(!ev_is_active(&sig->libevmess.w));
				ev_signal_start(loop->loop, &sig->libevmess.sig);
				if (!base->keep_loop_alive) ev_unref(loop->loop);
			}
			break;
		case LI_EVT_PREPARE:
			{
				liEventPrepare *prepare = li_event_prepare_from(base);
				LI_FORCE_ASSERT(!ev_is_active(&prepare->libevmess.w));
				ev_prepare_start(loop->loop, &prepare->libevmess.prepare);
				if (!base->keep_loop_alive) ev_unref(loop->loop);
			}
			break;
		case LI_EVT_CHECK:
			{
				liEventCheck *check = li_event_check_from(base);
				LI_FORCE_ASSERT(!ev_is_active(&check->libevmess.w));
				ev_check_start(loop->loop, &check->libevmess.check);
				if (!base->keep_loop_alive) ev_unref(loop->loop);
			}
			break;
		}
	}
}

INLINE void li_event_stop_(liEventBase *base) {
	liEventLoop *loop = base->link_watchers.data;

	if (!base->active) return;
	base->active = 0;

	LI_FORCE_ASSERT(NULL != base->callback);
	LI_FORCE_ASSERT(LI_EVT_NONE != base->type);

	if (NULL != loop) {
		switch (base->type) {
		case LI_EVT_NONE:
			break;
		case LI_EVT_IO:
			{
				liEventIO *io = li_event_io_from(base);
				LI_FORCE_ASSERT(ev_is_active(&io->libevmess.w));
				if (!base->keep_loop_alive) ev_ref(loop->loop);
				ev_io_stop(loop->loop, &io->libevmess.io);
			}
			break;
		case LI_EVT_TIMER:
			{
				liEventTimer *timer = li_event_timer_from(base);
				LI_FORCE_ASSERT(ev_is_active(&timer->libevmess.w));
				if (!base->keep_loop_alive) ev_ref(loop->loop);
				ev_timer_stop(loop->loop, &timer->libevmess.timer);
			}
			break;
		case LI_EVT_ASYNC:
			{
				liEventAsync *async = li_event_async_from(base);
				LI_FORCE_ASSERT(ev_is_active(&async->libevmess.w));
				if (!base->keep_loop_alive) ev_ref(loop->loop);
				ev_async_stop(loop->loop, &async->libevmess.async);
			}
			break;
		case LI_EVT_CHILD:
			{
				liEventChild *child = li_event_child_from(base);
				LI_FORCE_ASSERT(ev_is_active(&child->libevmess.w));
				if (!base->keep_loop_alive) ev_ref(loop->loop);
				ev_child_stop(loop->loop, &child->libevmess.child);
			}
			break;
		case LI_EVT_SIGNAL:
			{
				liEventSignal *sig = li_event_signal_from(base);
				LI_FORCE_ASSERT(ev_is_active(&sig->libevmess.w));
				if (!base->keep_loop_alive) ev_ref(loop->loop);
				ev_signal_stop(loop->loop, &sig->libevmess.sig);
			}
			break;
		case LI_EVT_PREPARE:
			{
				liEventPrepare *prepare = li_event_prepare_from(base);
				LI_FORCE_ASSERT(ev_is_active(&prepare->libevmess.w));
				if (!base->keep_loop_alive) ev_ref(loop->loop);
				ev_prepare_stop(loop->loop, &prepare->libevmess.prepare);
			}
			break;
		case LI_EVT_CHECK:
			{
				liEventCheck *check = li_event_check_from(base);
				LI_FORCE_ASSERT(ev_is_active(&check->libevmess.w));
				if (!base->keep_loop_alive) ev_ref(loop->loop);
				ev_check_stop(loop->loop, &check->libevmess.check);
			}
			break;
		}
	}
}

INLINE gboolean li_event_active_(liEventBase *base) {
	return base->active;
}


INLINE void li_event_clear_(liEventBase *base) {
	if (LI_EVT_NONE == base->type) return;

	if (li_event_attached_(base)) li_event_detach_(base);
	base->active = FALSE;
	base->callback = NULL;

	switch (base->type) {
	case LI_EVT_NONE:
		break;
	case LI_EVT_IO:
		{
			liEventIO *io = li_event_io_from(base);
			io->events = 0;
			ev_io_set(&io->libevmess.io, -1, 0);
			ev_set_cb(&io->libevmess.io, NULL);
		}
		break;
	case LI_EVT_TIMER:
		{
			liEventTimer *timer = li_event_timer_from(base);
			timer->libevmess.timer.repeat = 0;
			ev_set_cb(&timer->libevmess.timer, NULL);
		}
		break;
	case LI_EVT_ASYNC:
		{
			liEventAsync *async = li_event_async_from(base);
			ev_set_cb(&async->libevmess.async, NULL);
		}
		break;
	case LI_EVT_CHILD:
		{
			liEventChild *child = li_event_child_from(base);
			ev_child_set(&child->libevmess.child, -1, 0);
			ev_set_cb(&child->libevmess.child, NULL);
		}
		break;
	case LI_EVT_SIGNAL:
		{
			liEventSignal *sig = li_event_signal_from(base);
			ev_set_cb(&sig->libevmess.sig, NULL);
			ev_signal_set(&sig->libevmess.sig, 0);
		}
		break;
	case LI_EVT_PREPARE:
		{
			liEventPrepare *prepare = li_event_prepare_from(base);
			ev_set_cb(&prepare->libevmess.prepare, NULL);
		}
		break;
	case LI_EVT_CHECK:
		{
			liEventCheck *check = li_event_check_from(base);
			ev_set_cb(&check->libevmess.check, NULL);
		}
		break;
	}
	base->type = LI_EVT_NONE;
}

INLINE void li_event_set_callback_(liEventBase *base, liEventCallback callback) {
	base->callback = callback;
}

INLINE void li_event_set_keep_loop_alive_(liEventBase *base, gboolean keep_loop_alive) {
	liEventLoop *loop = base->link_watchers.data;
	unsigned int v = keep_loop_alive ? 1 : 0;
	if (v == base->keep_loop_alive) return;
	base->keep_loop_alive = v;

	if (NULL == loop || !base->active) return;

	if (v) {
		ev_ref(loop->loop);
	} else {
		ev_unref(loop->loop);
	}
}

INLINE int li_event_io_fd(liEventIO *io) {
	return io->libevmess.io.fd;
}

INLINE liEventIO* li_event_io_from(liEventBase *base) {
	LI_FORCE_ASSERT(LI_EVT_IO == base->type);
	return LI_CONTAINER_OF(base, liEventIO, base);
}

INLINE void li_event_timer_once(liEventTimer *timer, li_tstamp timeout) {
	li_event_stop(timer);
	timer->libevmess.timer.repeat = timeout;
	li_event_start(timer);
}

INLINE liEventTimer* li_event_timer_from(liEventBase *base) {
	LI_FORCE_ASSERT(LI_EVT_TIMER == base->type);
	return LI_CONTAINER_OF(base, liEventTimer, base);
}

INLINE void li_event_async_send(liEventAsync *async) {
	liEventLoop *loop = async->base.link_watchers.data;
	ev_async_send(loop->loop, &async->libevmess.async);
}

INLINE liEventAsync* li_event_async_from(liEventBase *base){
	LI_FORCE_ASSERT(LI_EVT_ASYNC == base->type);
	return LI_CONTAINER_OF(base, liEventAsync, base);
}

INLINE int li_event_child_pid(liEventChild *child) {
	return child->libevmess.child.pid;
}

INLINE int li_event_child_status(liEventChild *child) {
	return child->libevmess.child.rstatus;
}

INLINE liEventChild* li_event_child_from(liEventBase *base) {
	LI_FORCE_ASSERT(LI_EVT_CHILD == base->type);
	return LI_CONTAINER_OF(base, liEventChild, base);
}

INLINE int li_event_signal_signum(liEventSignal *sig) {
	return sig->libevmess.sig.signum;
}

INLINE liEventSignal* li_event_signal_from(liEventBase *base) {
	LI_FORCE_ASSERT(LI_EVT_SIGNAL == base->type);
	return LI_CONTAINER_OF(base, liEventSignal, base);
}

INLINE liEventPrepare* li_event_prepare_from(liEventBase *base) {
	LI_FORCE_ASSERT(LI_EVT_PREPARE == base->type);
	return LI_CONTAINER_OF(base, liEventPrepare, base);
}

INLINE liEventCheck* li_event_check_from(liEventBase *base) {
	LI_FORCE_ASSERT(LI_EVT_CHECK == base->type);
	return LI_CONTAINER_OF(base, liEventCheck, base);
}

#pragma clang diagnostic pop

#endif

#ifndef _LIGHTTPD_WORKER_H_
#define _LIGHTTPD_WORKER_H_

#ifndef _LIGHTTPD_BASE_H_
#error Please include <lighttpd/base.h> instead of this file
#endif

#include <lighttpd/tasklet.h>
#include <lighttpd/events.h>

struct lua_State;

typedef struct liStatistics liStatistics;
struct liStatistics {
	guint64 bytes_out;        /** bytes transfered, outgoing */
	guint64 bytes_in;         /** bytes transfered, incoming */

	guint64 requests;         /** processed requests */
	guint64 active_cons_cum;  /** cumulative value of active connections, updated once a second */

	guint64 actions_executed; /** actions executed */

	/* 5 seconds frame avg */
	guint64 requests_5s;
	guint64 requests_5s_diff;
	guint64 bytes_out_5s;
	guint64 bytes_out_5s_diff;
	guint64 bytes_in_5s;
	guint64 bytes_in_5s_diff;
	guint active_cons_5s;
	li_tstamp last_avg;

	/* peak values from 5s avg */
	struct {
		guint64 requests;
		guint64 bytes_out;
		guint64 bytes_in;
		guint active_cons;
	} peak;

	/* updated in timer */
	guint64 last_requests;
	double requests_per_sec;
	li_tstamp last_update;
};

typedef struct liWorkerTS liWorkerTS;
struct liWorkerTS {
	time_t last_generated;
	GString *str;
};

struct liWorker {
	liServer *srv;

	GThread *thread; /* managed by server.c */
	guint ndx;       /* worker index */

	liLuaState LL;

	liEventLoop loop;
	liEventPrepare loop_prepare;
	liEventAsync worker_stop_watcher, worker_stopping_watcher, worker_suspend_watcher, worker_exit_watcher;

	liLogWorkerData logs;

	guint connections_active; /** 0..con_act-1: active connections, con_act..used-1: free connections
	                            * use with atomic, read direct from local worker context
	                            */
	guint connections_active_max_5min; /** max() of active connections during the last 5 minutes */
	GArray *connections;      /** array of (connection*), use only from local worker context */
	li_tstamp connections_gc_ts;

	GString *tmp_str;         /**< can be used everywhere for local temporary needed strings */

	/* keep alive timeout queue */
	liEventTimer keep_alive_timer;
	GQueue keep_alive_queue;

	liWaitQueue io_timeout_queue;

	liWaitQueue throttle_queue;

	guint connection_load;    /** incremented by server_accept_cb, decremented by worker_con_put. use atomic access */

	GArray *timestamps_gmt; /** array of (worker_ts), use only from local worker context and through li_worker_current_timestamp(wrk, LI_GMTIME, ndx) */
	GArray *timestamps_local;

	/* incoming queues */
	/*  - new connections (after accept) */
	liEventAsync new_con_watcher;
	GAsyncQueue *new_con_queue;

	liServerStateWait wait_for_stop_connections;

	liEventTimer stats_watcher;
	liStatistics stats;

	/* collect framework */
	liEventAsync collect_watcher;
	GAsyncQueue *collect_queue;

	liTaskletPool *tasklets;

	liStatCache *stat_cache;

	liBuffer *network_read_buf; /** available buffer - steal it if you need it, can be NULL. refcount must be 1, no other references. */
};

LI_API liWorker* li_worker_new(liServer *srv, struct ev_loop *loop);
LI_API struct ev_loop* li_worker_free(liWorker *wrk);

LI_API void li_worker_run(liWorker *wrk);

/* stopped now, all connections down. exit loop soon */
LI_API void li_worker_stop(liWorker *context, liWorker *wrk);

/* start stopping. don't stop loop yet, connection handling on other workers might need all workers (mod_status) */
LI_API void li_worker_stopping(liWorker *context, liWorker *wrk);

LI_API void li_worker_suspend(liWorker *context, liWorker *wrk);
LI_API void li_worker_exit(liWorker *context, liWorker *wrk);

LI_API void li_worker_new_con(liWorker *ctx, liWorker *wrk, liSocketAddress remote_addr, int s, liServerSocket *srv_sock);

LI_API void li_worker_check_keepalive(liWorker *wrk);

LI_API GString* li_worker_current_timestamp(liWorker *wrk, liTimeFunc, guint format_ndx);

/* shutdown write and wait for eof before shutdown read and close */
LI_API void li_worker_add_closing_socket(liWorker *wrk, int fd);

/* internal function to recycle connection */
LI_API void li_worker_con_put(liConnection *con);

INLINE li_tstamp li_cur_ts(liWorker *wrk);

INLINE liWorker* li_worker_from_iostream(liIOStream *stream);
INLINE liWorker* li_worker_from_stream(liStream *stream);



/* inline implementations */

INLINE li_tstamp li_cur_ts(liWorker *wrk) {
	return li_event_now(&wrk->loop);
}

INLINE liWorker* li_worker_from_stream(liStream *stream) {
	if (NULL == stream->loop) return NULL;
	return LI_CONTAINER_OF(stream->loop, liWorker, loop);
}

INLINE liWorker* li_worker_from_iostream(liIOStream *stream) {
	liWorker* wrk;
	wrk = li_worker_from_stream(&stream->stream_in);
	if (NULL != wrk) return wrk;
	wrk = li_worker_from_stream(&stream->stream_out);
	if (NULL != wrk) return wrk;
	{
		liEventLoop *loop = li_event_get_loop(&stream->io_watcher);
		if (NULL == loop) return NULL;
		return LI_CONTAINER_OF(loop, liWorker, loop);
	}
}

#endif

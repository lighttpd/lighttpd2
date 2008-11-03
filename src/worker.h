#ifndef _LIGHTTPD_WORKER_H_
#define _LIGHTTPD_WORKER_H_

#ifndef _LIGHTTPD_BASE_H_
#error Please include "base.h" instead of this file
#endif

struct statistics_t;
typedef struct statistics_t statistics_t;

struct statistics_t {
	guint64 bytes_out;        /** bytes transfered, outgoing */
	guint64 bytes_in;         /** bytes transfered, incoming */

	guint64 requests;         /** processed requests */
	guint64 active_cons_cum;  /** cummulative value of active connections, updated once a second */

	guint64 actions_executed; /** actions executed */

	/* 5 seconds frame avg */
	guint64 requests_5s;
	guint64 requests_5s_diff;
	guint64 bytes_out_5s;
	guint64 bytes_out_5s_diff;
	guint64 bytes_in_5s;
	guint64 bytes_in_5s_diff;
	guint active_cons_5s;
	time_t last_avg;

	/* updated in timer */
	guint64 last_requests;
	double requests_per_sec;
	ev_tstamp last_update;
};

#define CUR_TS(wrk) ((time_t)ev_now((wrk)->loop))

/* only locks if there is more than one worker */
#define WORKER_LOCK(srv, lock) \
	if ((srv)->worker_count > 1) g_static_rec_mutex_lock(lock)
#define WORKER_UNLOCK(srv, lock) \
	if ((srv)->worker_count > 1) g_static_rec_mutex_unlock(lock)

struct worker {
	struct server *srv;

	GThread *thread; /* managed by server.c */
	guint ndx;       /* worker index */

	struct ev_loop *loop;
	ev_prepare loop_prepare;
	ev_check loop_check;
	ev_async worker_stop_watcher, worker_exit_watcher;

	guint connections_active; /** 0..con_act-1: active connections, con_act..used-1: free connections
	                            * use with atomic, read direct from local worker context
	                            */
	GArray *connections;      /** array of (connection*), use only from local worker context */

	GQueue closing_sockets;   /** wait for EOF before shutdown(SHUT_RD) and close() */

	GString *tmp_str;         /**< can be used everywhere for local temporary needed strings */

	/* keep alive timeout queue */
	ev_timer keep_alive_timer;
	GQueue keep_alive_queue;

	guint connection_load;    /** incremented by server_accept_cb, decremented by worker_con_put. use atomic access */

	time_t last_generated_date_ts;
	GString *ts_date_str;     /**< use worker_current_timestamp(wrk) */

	/* incoming queues */
	/*  - new connections (after accept) */
	ev_async new_con_watcher;
	GAsyncQueue *new_con_queue;

	ev_timer stats_watcher;
	statistics_t stats;

	/* collect framework */
	ev_async collect_watcher;
	GAsyncQueue *collect_queue;
};

LI_API worker* worker_new(struct server *srv, struct ev_loop *loop);
LI_API void worker_free(worker *wrk);

LI_API void worker_run(worker *wrk);
LI_API void worker_stop(worker *context, worker *wrk);
LI_API void worker_exit(worker *context, worker *wrk);

LI_API void worker_new_con(worker *ctx, worker *wrk, sock_addr *remote_addr, int s);

LI_API void worker_check_keepalive(worker *wrk);

LI_API GString *worker_current_timestamp(worker *wrk);

/* shutdown write and wait for eof before shutdown read and close */
LI_API void worker_add_closing_socket(worker *wrk, int fd);

#endif

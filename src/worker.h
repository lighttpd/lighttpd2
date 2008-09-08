#ifndef _LIGHTTPD_WORKER_H_
#define _LIGHTTPD_WORKER_H_

struct worker;
typedef struct worker worker;

struct server;

#include "settings.h"

#define CUR_TS(wrk) ((time_t)ev_now((wrk)->loop))

struct worker {
	struct server *srv;

	struct ev_loop *loop;
	ev_prepare loop_prepare;
	ev_check loop_check;

	GQueue closing_sockets;   /** wait for EOF before shutdown(SHUT_RD) and close() */

	GString *tmp_str;         /**< can be used everywhere for local temporary needed strings */

	/* keep alive timeout queue */
	ev_timer keep_alive_timer;
	GQueue keep_alive_queue;

	guint connection_load;

	time_t last_generated_date_ts;
	GString *ts_date_str;     /**< use server_current_timestamp(srv) */
};

LI_API worker* worker_new(struct server *srv, struct ev_loop *loop);
LI_API void worker_free(worker *wrk);

LI_API void worker_run(worker *wrk);

LI_API void worker_check_keepalive(worker *wrk);

LI_API GString *worker_current_timestamp(worker *wrk);

/* shutdown write and wait for eof before shutdown read and close */
LI_API void worker_add_closing_socket(worker *wrk, int fd);

#endif

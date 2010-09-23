#ifndef _LIGHTTPD_CONNECTION_H_
#define _LIGHTTPD_CONNECTION_H_

#ifndef _LIGHTTPD_BASE_H_
#error Please include <lighttpd/base.h> instead of this file
#endif

typedef enum {
	/** unused */
	LI_CON_STATE_DEAD,

	/** waiting for new input after first request */
	LI_CON_STATE_KEEP_ALIVE,

	/** after the connect, the request is initialized */
	LI_CON_STATE_REQUEST_START,

	/** loop in the read-request-header until the full header is received */
	LI_CON_STATE_READ_REQUEST_HEADER,

	/** handle in main virtual request */
	LI_CON_STATE_HANDLE_MAINVR,

	/** write remaining bytes from raw_out, mainvr finished (or not started) */
	LI_CON_STATE_WRITE,
} liConnectionState;

struct liConnection {
	guint idx; /** index in connection table */
	liServer *srv;
	liWorker *wrk;
	liServerSocket *srv_sock;
	gpointer srv_sock_data; /** private data for custom sockets (ssl) */

	liConInfo info;

	liConnectionState state;
	gboolean response_headers_sent, expect_100_cont;

	liChunkQueue *raw_in, *raw_out;
	liChunkQueue *in, *out;    /* link to mainvr->in/out */
	liBuffer *raw_in_buffer;

	liVRequest *mainvr;
	liHttpRequestCtx req_parser_ctx;

	ev_tstamp ts_started; /* when connection was started, not a (v)request */

	/* Keep alive timeout data */
	struct {
		GList *link;
		ev_tstamp timeout;
		guint max_idle;
		ev_timer watcher;
	} keep_alive_data;
	guint keep_alive_requests;

	ev_io sock_watcher;
	gboolean can_read, can_write;

	/* I/O timeout data */
	liWaitQueueElem io_timeout_elem;
};

/* Internal functions */
LI_API liConnection* li_connection_new(liWorker *wrk);
/** Free dead connections */
LI_API void li_connection_free(liConnection *con);
/** close connection */
LI_API void li_connection_reset(liConnection *con);

/** aborts an active connection, calls all plugin cleanup handlers */
LI_API void li_connection_error(liConnection *con); /* used in worker.c */

LI_API void li_connection_start(liConnection *con, liSocketAddress remote_addr, int s, liServerSocket *srv_sock);

/* public function */
LI_API gchar *li_connection_state_str(liConnectionState state);

/* returns NULL if the vrequest doesn't belong to a liConnection* object */
LI_API liConnection* li_connection_from_vrequest(liVRequest *vr);

LI_API void connection_handle_io(liConnection *con);

#endif

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

	liConnectionState state;
	gboolean response_headers_sent, expect_100_cont;

	liChunkQueue *raw_in, *raw_out;
	liChunkQueue *in, *out;    /* link to mainvr->in/out */

	ev_io sock_watcher;
	liSocketAddress remote_addr, local_addr;
	GString *remote_addr_str, *local_addr_str;
	gboolean is_ssl, keep_alive;

	liVRequest *mainvr;
	liHttpRequestCtx req_parser_ctx;

	/* Keep alive timeout data */
	struct {
		GList *link;
		ev_tstamp timeout;
		guint max_idle;
		ev_timer watcher;
	} keep_alive_data;
	guint keep_alive_requests;

	/* I/O timeout data */
	liWaitQueueElem io_timeout_elem;

	/* I/O throttling */
	gboolean throttled; /* TRUE if connection is throttled */
	struct {
		struct {
			liThrottlePool *ptr; /* NULL if not in any throttling pool */
			GList lnk;
			gboolean queued;
			gint magazine;
		} pool;
		struct {
			liThrottlePool *ptr; /* pool for per-ip throttling, NULL if not limited by ip */
			GList lnk;
			gboolean queued;
			gint magazine;
		} ip;
		struct {
			guint rate; /* maximum transfer rate in bytes per second, 0 if unlimited */
			gint magazine;
			ev_tstamp last_update;
		} con;
		liWaitQueueElem wqueue_elem;
	} throttle;

	ev_tstamp ts_started;

	struct {
		guint64 bytes_in; /* total number of bytes received */
		guint64 bytes_out; /* total number of bytes sent */
		ev_tstamp last_avg;
		guint64 bytes_in_5s; /* total number of bytes received at last 5s interval */
		guint64 bytes_out_5s; /* total number of bytes sent at last 5s interval */
		guint64 bytes_in_5s_diff; /* diff between bytes received at 5s interval n and interval n-1 */
		guint64 bytes_out_5s_diff; /* diff between bytes sent at 5s interval n and interval n-1 */
	} stats;
};

/* Internal functions */
LI_API liConnection* li_connection_new(liWorker *wrk);
/** Free dead connections */
LI_API void li_connection_free(liConnection *con);
/** close connection */
LI_API void li_connection_reset(liConnection *con);

/** aborts an active connection, calls all plugin cleanup handlers */
LI_API void li_connection_error(liConnection *con); /* used in worker.c */

/* public function */
LI_API gchar *li_connection_state_str(liConnectionState state);

#endif

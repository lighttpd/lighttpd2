#ifndef _LIGHTTPD_CONNECTION_H_
#define _LIGHTTPD_CONNECTION_H_

#ifndef _LIGHTTPD_BASE_H_
#error Please include <lighttpd/base.h> instead of this file
#endif

typedef enum {
	/** unused */
	LI_CON_STATE_DEAD,

	/** closed (or "closing") */
	LI_CON_STATE_CLOSE,

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

	/** connection was upgraded */
	LI_CON_STATE_UPGRADED
} liConnectionState;
#define LI_CON_STATE_LAST LI_CON_STATE_UPGRADED
/* update mod_status too */

typedef struct liConnectionSocketCallbacks liConnectionSocketCallbacks;
typedef struct liConnectionSocket liConnectionSocket;
typedef struct liConnectionSimpleTcpState liConnectionSimpleTcpState;

struct liConnectionSocketCallbacks {
	void (*finish)(liConnection *con, gboolean aborted);
	liThrottleState* (*throttle_out)(liConnection *con);
	liThrottleState* (*throttle_in)(liConnection *con);
};

struct liConnectionSocket {
	gpointer data; /** private data (simple tcp, ssl, ...) */
	const liConnectionSocketCallbacks *callbacks;

	liStream *raw_in, *raw_out;
};

struct liConnection {
	guint idx; /** index in connection table, -1 if not active */
	liServer *srv;
	liWorker *wrk;
	liServerSocket *srv_sock;
	liConnectionSocket con_sock;

	liConInfo info;

	liConnectionState state;
	gboolean response_headers_sent, expect_100_cont, out_has_all_data;

	liStream in, out;
	liFilterChunkedDecodeState in_chunked_decode_state;
	liConnectionProxyProtocolFilter proxy_protocol_filter;

	liVRequest *mainvr;
	liHttpRequestCtx req_parser_ctx;

	li_tstamp ts_started; /* when connection was started, not a (v)request */

	/* Keep alive timeout data */
	struct {
		GList *link;
		li_tstamp timeout;
		guint max_idle;
		liEventTimer watcher;
	} keep_alive_data;
	guint keep_alive_requests;

	/* I/O read timeout data */
	liWaitQueueElem io_timeout_elem;

	liJob job_reset;
};

struct liConnectionSimpleTcpState {
	liBuffer *read_buffer;
};

/* Internal functions */
LI_API liConnection* li_connection_new(liWorker *wrk);
/** Free dead connections */
LI_API void li_connection_free(liConnection *con);
/* close connection (for worker keep-alive timeout) */
LI_API void li_connection_reset(liConnection *con);

/* update whether we're waiting for io timeouts */
LI_API void li_connection_update_io_wait(liConnection *con);

/** aborts an active connection, calls all plugin cleanup handlers */
LI_API void li_connection_error(liConnection *con); /* used in worker.c */

LI_API void li_connection_start(liConnection *con, liSocketAddress remote_addr, int s, liServerSocket *srv_sock);

/* public function */
LI_API gchar *li_connection_state_str(liConnectionState state);

/* returns NULL if the vrequest doesn't belong to a liConnection* object */
LI_API liConnection* li_connection_from_vrequest(liVRequest *vr);


/****************************************************************/
/* IO backend stuff (simple tcp (or unix), tls implementations) */
/****************************************************************/

/* call after IO send operations if con->out_has_all_data and out queues are empty */
LI_API void li_connection_request_done(liConnection *con);

/* call after successful io
 * li_connection_simple_tcp takes care of this for you.
 */
LI_API void li_connection_update_io_timeout(liConnection *con);

INLINE void li_connection_simple_tcp_init(liConnectionSimpleTcpState *state);

/* handles IOStream events for a connection; updates transferred bytes and io timeouts;
 * *pcon is needed to handle cases then the connections gets reset while handling io stuff
 * NULL == *pcon is ok - it won't update transferred bytes and io timeouts then.
 * closes outgoing stream on reading EOF
 *
 * clear state by calling with LI_IOSTREAM_DESTROY (through li_iostream_release
 * and the liIOStreamCB forwarding to li_connection_simple_tcp).
 */
LI_API void li_connection_simple_tcp(liConnection **pcon, liIOStream *stream, liConnectionSimpleTcpState *state, liIOStreamEvent event);

/* default for liServerSocket->new_cb - plain HTTP */
LI_API gboolean li_connection_http_new(liConnection *con, int fd);

/******************************************************/

/********************
 * Inline functions *
 ********************/

INLINE void li_connection_simple_tcp_init(liConnectionSimpleTcpState *state) {
	state->read_buffer = NULL;
}

#endif

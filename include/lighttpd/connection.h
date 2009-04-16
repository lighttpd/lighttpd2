#ifndef _LIGHTTPD_CONNECTION_H_
#define _LIGHTTPD_CONNECTION_H_

#ifndef _LIGHTTPD_BASE_H_
#error Please include <lighttpd/base.h> instead of this file
#endif

typedef enum {
	/** unused */
	CON_STATE_DEAD,

	/** waiting for new input after first request */
	CON_STATE_KEEP_ALIVE,

	/** after the connect, the request is initialized */
	CON_STATE_REQUEST_START,

	/** loop in the read-request-header until the full header is received */
	CON_STATE_READ_REQUEST_HEADER,

	/** handle in main virtual request */
	CON_STATE_HANDLE_MAINVR,

	/** write remaining bytes from raw_out, mainvr finished (or not started) */
	CON_STATE_WRITE,
} connection_state_t;

struct connection {
	guint idx; /** index in connection table */
	server *srv;
	worker *wrk;
	server_socket *srv_sock;

	connection_state_t state;
	gboolean response_headers_sent, expect_100_cont;

	chunkqueue *raw_in, *raw_out;
	chunkqueue *in, *out;    /* link to mainvr->in/out */

	ev_io sock_watcher;
	sockaddr_t remote_addr;
	GString *remote_addr_str;
	gboolean is_ssl, keep_alive;

	vrequest *mainvr;
	http_request_ctx req_parser_ctx;

	/* Keep alive timeout data */
	struct {
		GList *link;
		ev_tstamp timeout;
		guint max_idle;
		ev_timer watcher;
	} keep_alive_data;
	guint keep_alive_requests;

	/* I/O timeout data */
	waitqueue_elem io_timeout_elem;

	/* I/O throttling */
	gboolean throttled; /* TRUE if connection is throttled */
	struct {
		struct {
			throttle_pool_t *ptr; /* NULL if not in any throttling pool */
			GList lnk;
			gboolean queued;
			gint magazine;
		} pool;
		struct {
			throttle_pool_t *ptr; /* pool for per-ip throttling, NULL if not limited by ip */
			GList lnk;
			gboolean queued;
			gint magazine;
		} ip;
		struct {
			guint rate; /* maximum transfer rate in bytes per second, 0 if unlimited */
			gint magazine;
			ev_tstamp last_update;
		} con;
		waitqueue_elem wqueue_elem;
	} throttle;

	ev_tstamp ts;

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

LI_API connection* connection_new(worker *wrk);
LI_API void connection_reset(connection *con);
LI_API void connection_reset_keep_alive(connection *con);
LI_API void connection_free(connection *con);

LI_API void connection_error(connection *con);
LI_API void connection_internal_error(connection *con);

LI_API void connection_handle_direct(connection *con);
LI_API void connection_handle_indirect(connection *con, plugin *p);

LI_API gchar *connection_state_str(connection_state_t state);

#endif

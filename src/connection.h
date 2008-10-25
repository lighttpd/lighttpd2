#ifndef _LIGHTTPD_CONNECTION_H_
#define _LIGHTTPD_CONNECTION_H_

#include "base.h"

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

	connection_state_t state;
	gboolean response_headers_sent, expect_100_cont;

	chunkqueue *raw_in, *raw_out;
	chunkqueue *in, *out;    /* link to mainvr->in/out */

	ev_io sock_watcher;
	sock_addr remote_addr, local_addr;
	GString *remote_addr_str, *local_addr_str;
	gboolean is_ssl, keep_alive;

	option_value *options;

	vrequest *mainvr;
	http_request_ctx req_parser_ctx;

	struct log_t *log;
	gint log_level;

	/* Keep alive timeout data */
	struct {
		GList *link;
		ev_tstamp timeout;
		guint max_idle;
		ev_timer watcher;
	} keep_alive_data;
};

LI_API connection* connection_new(worker *wrk);
LI_API void connection_reset(connection *con);
LI_API void connection_reset_keep_alive(connection *con);
LI_API void connection_free(connection *con);

LI_API void connection_error(connection *con);

LI_API void connection_handle_direct(connection *con);
LI_API void connection_handle_indirect(connection *con, plugin *p);

#endif

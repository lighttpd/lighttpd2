#ifndef _LIGHTTPD_CONNECTION_H_
#define _LIGHTTPD_CONNECTION_H_

#include "base.h"

typedef enum {
	/** after the connect, the request is initialized, keep-alive starts here again */
	CON_STATE_REQUEST_START,

	/** loop in the read-request-header until the full header is received */
	CON_STATE_READ_REQUEST_HEADER,
	/** validate the request-header */
	CON_STATE_VALIDATE_REQUEST_HEADER,

	/** find a handler for the request; there are two ways to produce responses:
	  *  - direct response: for things like errors/auth/redirect
	  *    just set the status code, perhaps fill in some headers,
	  *    append your content (if any) to the queue and do:
	  *      connection_handle_direct(srv, con);
	  *    this moves into the CON_STATE_HANDLE_RESPONSE_HEADER
	  *    request body gets dropped
	  *  - indirect response: you register your plugin as the content handler:
	  *      connection_handle_indirect(srv, con, plugin);
	  *    this moves into the CON_STATE_READ_REQUEST_CONTENT state automatically
	  *    as soon as you build the response headers (e.g. from a backend),
	  *    change to the CON_STATE_HANDLE_RESPONSE_HEADER state:
	  *      connection_set_state(srv, con, CON_STATE_HANDLE_RESPONSE_HEADER);
	  */
	CON_STATE_HANDLE_REQUEST_HEADER,

	/** start forwarding the request content to the handler;
	  * any filter for the request content must register before a handler
	  * for the response content registers
	  */
	CON_STATE_READ_REQUEST_CONTENT,

	/** response headers available; this is were compress/deflate should register
	  * their response content filters
	  * if all actions are done (or one returns HANDLER_FINISHED) we start
	  * writing the response content
	  */
	CON_STATE_HANDLE_RESPONSE_HEADER,

	/** start sending content */
	CON_STATE_WRITE_RESPONSE,

	/** successful request, connection closed - final state */
	CON_STATE_RESPONSE_END,

	/** connection reset by peer - final state */
	CON_STATE_CLOSE,

	/** fatal error, connection closed - final state */
	CON_STATE_ERROR
} connection_state_t;

struct connection_socket;
typedef struct connection_socket connection_socket;

struct connection_socket {
	server *srv;
	connection *con;
	ev_io watcher;
};

struct connection {
	guint idx; /** index in connection table */
	connection_state_t state;
	gboolean response_headers_sent, expect_100_cont;

	chunkqueue *raw_in, *raw_out;
	chunkqueue *in, *out;

	connection_socket sock;
	sock_addr remote_addr, local_addr;
	GString *remote_addr_str, *local_addr_str;
	gboolean is_ssl, keep_alive;

	action_stack action_stack;

	gpointer *options;

	request request;
	physical physical;

	response response;

	plugin *content_handler;

	struct log_t *log;
	gint log_level;
};

LI_API connection* connection_new(server *srv);
LI_API void connection_reset(server *srv, connection *con);
LI_API void connection_free(server *srv, connection *con);

LI_API void connection_set_state(server *srv, connection *con, connection_state_t state);
LI_API void connection_state_machine(server *srv, connection *con);

LI_API void connection_handle_direct(server *srv, connection *con);
LI_API void connection_handle_indirect(server *srv, connection *con, plugin *p);

#endif

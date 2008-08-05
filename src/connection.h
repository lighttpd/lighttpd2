#ifndef _LIGHTTPD_CONNECTION_H_
#define _LIGHTTPD_CONNECTION_H_

#include "base.h"

typedef enum {
	CON_STATE_REQUEST_START,            /** after the connect, the request is initialized, keep-alive starts here again */
	CON_STATE_READ_REQUEST_HEADER,      /** loop in the read-request-header until the full header is received */
	CON_STATE_VALIDATE_REQUEST_HEADER,  /** validate the request-header */
	CON_STATE_HANDLE_RESPONSE,          /** find a handler for the request */
	CON_STATE_RESPONSE_END,             /** successful request, connection closed */
	CON_STATE_ERROR,                    /** fatal error, connection closed */
	CON_STATE_CLOSE                     /** connection reset by peer */
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

	chunkqueue *raw_in, *raw_out;
	chunkqueue *in, *out;

	connection_socket sock;
	sock_addr remote_addr, local_addr;
	GString *remote_addr_str, *local_addr_str;
	gboolean is_ssl;

	action_stack action_stack;

	gpointer *options; /* TODO */

	request request;
	physical physical;

	struct log_t *log;
	gint log_level;
};

LI_API connection* connection_new(server *srv);
LI_API void connection_reset(server *srv, connection *con);
LI_API void connection_free(server *srv, connection *con);

LI_API void connection_set_state(server *srv, connection *con, connection_state_t state);

#endif

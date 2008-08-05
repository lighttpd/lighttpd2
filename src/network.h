#ifndef _LIGHTTPD_NETWORK_H_
#define _LIGHTTPD_NETWORK_H_

#include "base.h"

typedef enum {
	NETWORK_STATUS_SUCCESS,
	NETWORK_STATUS_FATAL_ERROR,
	NETWORK_STATUS_CONNECTION_CLOSE,
	NETWORK_STATUS_WAIT_FOR_EVENT,
	NETWORK_STATUS_WAIT_FOR_AIO_EVENT,
	NETWORK_STATUS_WAIT_FOR_FD,
} network_status_t;

LI_API network_status_t network_write(server *srv, connection *con, int fd, chunkqueue *cq);
LI_API network_status_t network_read(server *srv, connection *con, int fd, chunkqueue *cq);

#endif

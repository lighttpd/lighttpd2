#ifndef _LIGHTTPD_NETWORK_H_
#define _LIGHTTPD_NETWORK_H_

#include "base.h"

typedef enum {
	NETWORK_STATUS_SUCCESS,             /**< some IO was actually done (read/write) or cq was empty for write */
	NETWORK_STATUS_FATAL_ERROR,
	NETWORK_STATUS_CONNECTION_CLOSE,
	NETWORK_STATUS_WAIT_FOR_EVENT,      /**< read/write returned -1 with errno=EAGAIN/EWOULDBLOCK; no real IO was done */
	NETWORK_STATUS_WAIT_FOR_AIO_EVENT,  /**< nothing done yet, read/write will be done somewhere else */
	NETWORK_STATUS_WAIT_FOR_FD,         /**< need a fd to open a file */
} network_status_t;

/** repeats write after EINTR */
LI_API ssize_t net_write(int fd, void *buf, ssize_t nbyte);

/** repeats read after EINTR */
LI_API ssize_t net_read(int fd, void *buf, ssize_t nbyte);

LI_API network_status_t network_write(server *srv, connection *con, int fd, chunkqueue *cq);
LI_API network_status_t network_read(server *srv, connection *con, int fd, chunkqueue *cq);

/* write backends */
LI_API network_status_t network_backend_write(server *srv, connection *con, int fd, chunkqueue *cq);
LI_API network_status_t network_backend_writev(server *srv, connection *con, int fd, chunkqueue *cq);

#define NETWORK_FALLBACK(f) do { \
	network_status_t res; \
	switch(res = f(srv, con, fd, cq)) { \
		case NETWORK_STATUS_SUCCESS: \
			break; \
		default: \
			return res; \
	} \
} while(0)

#endif

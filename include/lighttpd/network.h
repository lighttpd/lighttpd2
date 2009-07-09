#ifndef _LIGHTTPD_NETWORK_H_
#define _LIGHTTPD_NETWORK_H_

#ifndef _LIGHTTPD_BASE_H_
#error Please include <lighttpd/base.h> instead of this file
#endif

#if defined(USE_LINUX_SENDFILE) || defined(USE_FREEBSD_SENDFILE) || defined(USE_SOLARIS_SENDFILEV) || defined(USE_OSX_SENDFILE)
# define USE_SENDFILE
#endif

typedef enum {
	LI_NETWORK_STATUS_SUCCESS,             /**< some IO was actually done (read/write) or cq was empty for write */
	LI_NETWORK_STATUS_FATAL_ERROR,
	LI_NETWORK_STATUS_CONNECTION_CLOSE,
	LI_NETWORK_STATUS_WAIT_FOR_EVENT,      /**< read/write returned -1 with errno=EAGAIN/EWOULDBLOCK; no real IO was done
	                                            internal: some io may be done */
	LI_NETWORK_STATUS_WAIT_FOR_AIO_EVENT   /**< nothing done yet, read/write will be done somewhere else */
} liNetworkStatus;

/** repeats write after EINTR */
LI_API ssize_t li_net_write(int fd, void *buf, ssize_t nbyte);

/** repeats read after EINTR */
LI_API ssize_t li_net_read(int fd, void *buf, ssize_t nbyte);

LI_API liNetworkStatus li_network_write(liVRequest *vr, int fd, liChunkQueue *cq, goffset write_max);
LI_API liNetworkStatus li_network_read(liVRequest *vr, int fd, liChunkQueue *cq);

/* use writev for mem chunks, buffered read/write for files */
LI_API liNetworkStatus li_network_write_writev(liVRequest *vr, int fd, liChunkQueue *cq, goffset *write_max);

#ifdef USE_SENDFILE
/* use sendfile for files, writev for mem chunks */
LI_API liNetworkStatus li_network_write_sendfile(liVRequest *vr, int fd, liChunkQueue *cq, goffset *write_max);
#endif

/* write backends */
LI_API liNetworkStatus li_network_backend_write(liVRequest *vr, int fd, liChunkQueue *cq, goffset *write_max);
LI_API liNetworkStatus li_network_backend_writev(liVRequest *vr, int fd, liChunkQueue *cq, goffset *write_max);

#define LI_NETWORK_FALLBACK(f, write_max) do { \
	liNetworkStatus res; \
	switch(res = f(vr, fd, cq, write_max)) { \
		case LI_NETWORK_STATUS_SUCCESS: \
			break; \
		default: \
			return res; \
	} \
} while(0)

#endif

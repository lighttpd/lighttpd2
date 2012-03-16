#ifndef _LIGHTTPD_NETWORK_H_
#define _LIGHTTPD_NETWORK_H_

#ifndef _LIGHTTPD_BASE_H_
#error Please include <lighttpd/base.h> instead of this file
#endif

#if defined(USE_LINUX_SENDFILE) || defined(USE_FREEBSD_SENDFILE) || defined(USE_SOLARIS_SENDFILEV) || defined(USE_OSX_SENDFILE)
# define USE_SENDFILE
#endif

#define LI_NETWORK_ERROR li_network_error_quark()
LI_API GQuark li_network_error_quark(void);

/** repeats write after EINTR */
LI_API ssize_t li_net_write(int fd, void *buf, ssize_t nbyte);

/** repeats read after EINTR */
LI_API ssize_t li_net_read(int fd, void *buf, ssize_t nbyte);

LI_API liNetworkStatus li_network_write(int fd, liChunkQueue *cq, goffset write_max, GError **err);
LI_API liNetworkStatus li_network_read(int fd, liChunkQueue *cq, liBuffer **buffer, GError **err);

/* use writev for mem chunks, buffered read/write for files */
LI_API liNetworkStatus li_network_write_writev(int fd, liChunkQueue *cq, goffset *write_max, GError **err);

#ifdef USE_SENDFILE
/* use sendfile for files, writev for mem chunks */
LI_API liNetworkStatus li_network_write_sendfile(int fd, liChunkQueue *cq, goffset *write_max, GError **err);
#endif

/* write backends */
LI_API liNetworkStatus li_network_backend_write(int fd, liChunkQueue *cq, goffset *write_max, GError **err);
LI_API liNetworkStatus li_network_backend_writev(int fd, liChunkQueue *cq, goffset *write_max, GError **err);

#define LI_NETWORK_FALLBACK(f, write_max) do { \
	liNetworkStatus res; \
	switch(res = f(fd, cq, write_max, err)) { \
		case LI_NETWORK_STATUS_SUCCESS: \
			break; \
		default: \
			return res; \
	} \
} while(0)

#endif

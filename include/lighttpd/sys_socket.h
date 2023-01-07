#ifndef SYS_SOCKET_H
#define SYS_SOCKET_H

#ifdef HAVE_CONFIG_H
#include <lighttpd/config.h>
#endif

#ifdef _WIN32
#ifndef FD_SETSIZE
/* By default this is 64 */
#define FD_SETSIZE 4096
#endif /* FD_SETSIZE */
#include <winsock2.h>
#include <ws2tcpip.h>
/* #include <wspiapi.h> */
/* #define HAVE_IPV6 -- not until we've resolved the inet_ntop issue. */

#define ECONNRESET WSAECONNRESET
#define EINPROGRESS WSAEINPROGRESS
#define EALREADY WSAEALREADY
#define ENOTCONN WSAENOTCONN
#define EWOULDBLOCK WSAEWOULDBLOCK
#define ECONNABORTED WSAECONNABORTED
#define ECONNREFUSED WSAECONNREFUSED
#define EHOSTUNREACH WSAEHOSTUNREACH
#define ioctl ioctlsocket
#define hstrerror(x) ""
#define STDIN_FILENO 0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2
#ifndef __MINGW32__
#define ssize_t int
#endif /* __MINGW32__ */

#define sockread( fd, buf, bytes ) recv( fd, buf, bytes, 0 )

LI_API const char * inet_ntop(int af, const void *src, char *dst, socklen_t cnt);
int inet_aton(const char *cp, struct in_addr *inp);
#define HAVE_INET_ADDR
#undef HAVE_INET_ATON

#else /* _WIN32 */
#include <sys/types.h> /* required by netinet/tcp.h on FreeBSD */
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/un.h>
#include <arpa/inet.h>

#ifndef SUN_LEN
#define SUN_LEN(su) \
        (sizeof(*(su)) - sizeof((su)->sun_path) + strlen((su)->sun_path))
#endif /* SUN_LEN */

#define sockread( fd, buf, bytes ) read( fd, buf, bytes )
#define closesocket(x) close(x)

#include <netdb.h>
#endif /* !_WIN32 */

#ifdef HAVE_INET_NTOP
/* only define it if it isn't defined yet */
#ifndef HAVE_IPV6
#define HAVE_IPV6
#endif /* HAVE_IPV6 */
#endif /* HAVE_INET_NTOP */

typedef union liSockAddrPtr liSockAddrPtr;
union liSockAddrPtr {
	char *raw;

#ifdef HAVE_IPV6
	struct sockaddr_in6 *ipv6;
#endif
	struct sockaddr_in *ipv4;
#ifdef HAVE_SYS_UN_H
	struct sockaddr_un *un;
#endif
	struct sockaddr *plain;

#ifdef HAVE_SOCKADDR_STORAGE
	struct sockaddr_storage *storage;
#endif
};

typedef union liSockAddrStorage liSockAddrStorage;
union liSockAddrStorage {
#ifdef HAVE_IPV6
	struct sockaddr_in6 ipv6;
#endif
	struct sockaddr_in ipv4;
#ifdef HAVE_SYS_UN_H
	struct sockaddr_un un;
#endif
	struct sockaddr plain;

#ifdef HAVE_SOCKADDR_STORAGE
	struct sockaddr_storage storage;
#endif
};

typedef struct liSocketAddress liSocketAddress;
struct liSocketAddress {
	socklen_t len;
	/*
	 * address union pointer
	 *
	 * as we only allocate storage for the target address family,
	 * the compiler complains when we use a pointer to a "storage"
	 * (big size) address type and dereference it - as the allocation
	 * is too small (even if we then only access allocated parts
	 * within the storage type).
	 * So use type punning through union of pointers (C11 allows this;
	 * it doesn't have the concept of "active" members as C++ does).
	 */
	liSockAddrPtr addr_up;
};

#endif

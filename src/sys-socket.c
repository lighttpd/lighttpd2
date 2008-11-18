#include <lighttpd/base.h>
#include <lighttpd/sys-socket.h>

#ifndef HAVE_INET_ATON
/* win32 has inet_addr instead if inet_aton */
# ifdef HAVE_INET_ADDR
int inet_aton(const char *cp, struct in_addr *inp) {
    struct in_addr a;

    a.s_addr = inet_addr(cp);

    if (INADDR_NONE == a.s_addr) {
        return 0;
    }

    inp->s_addr = a.s_addr;

    return 1;
}
# else
#  error no inet_aton emulation found
# endif

#endif

#ifdef _WIN32

#include <winsock2.h>

/* windows doesn't have inet_ntop */

/*
I have to look into this more. WSAAddressToString takes a sockaddr structure, which includes
the port number, so I must first test this stuff more carefully. For now, no IPV6 on windows.
You will notice that HAVE_IPV6 is never true for win32.
*/

const char *inet_ntop(int af, const void *src, char *dst, socklen_t cnt)
{
	/* WSAAddressToString takes a full sockaddr, while inet_ntop only takes the address */
	struct sockaddr_in sock4;
	struct sockaddr_in6 sock6;
	DWORD addrLen = cnt;
	int err = 0;

	/* src is either an in_addr or an in6_addr. */
	const struct in_addr *src4 = (const struct in_addr*) src;
	const struct in6_addr *src6 = (const struct in6_addr*) src;

	int ipv6 = af == AF_INET6;

	/* DebugBreak(); */

	if ( ipv6 )
	{
		sock6.sin6_family = AF_INET6;
		sock6.sin6_port = 0;
		sock6.sin6_addr = *src6;
	}
	else
	{
		sock4.sin_family = AF_INET;
		sock4.sin_port = 0;
		sock4.sin_addr = *src4;
	}

	err = WSAAddressToStringA(
		ipv6 ? (LPSOCKADDR) &sock6 : (LPSOCKADDR) &sock4,
		ipv6 ? sizeof(sock6) : sizeof(sock4),
		NULL,
		dst, &addrLen );
	return err == 0 ? dst : NULL;
}


#endif

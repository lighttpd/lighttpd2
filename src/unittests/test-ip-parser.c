
#include <lighttpd/base.h>

#define perror(msg) g_error("(%s:%i) %s failed: %s", __FILE__, __LINE__, msg, g_strerror(errno))


typedef struct {
	struct {
		guint32 addr;
		guint32 networkmask;
		guint16 port;
	} ipv4;
	struct {
		guint8 addr[16];
		guint network;
		guint16 port;
	} ipv6;
	struct {
		GString *path;
	} unix_socket;
} netrange;

static void test_ipv4_net1(void) {
	netrange range;
	liSocketAddress addr;
	const char str0[] = "0.0.0.0/0:80";
	GString str1 = li_const_gstring(CONST_STR_LEN("127.0.0.1"));
	struct sockaddr_in *ipv4;

	g_assert(!li_parse_ipv6(str0, range.ipv6.addr, &range.ipv6.network, &range.ipv6.port));
	g_assert(li_parse_ipv4(str0, &range.ipv4.addr, &range.ipv4.networkmask, &range.ipv4.port));

	g_assert_cmpuint(range.ipv4.addr, ==, 0);
	g_assert_cmpuint(range.ipv4.networkmask, ==, 0);
	g_assert_cmpuint(range.ipv4.port, ==, 80);

	addr = li_sockaddr_from_string(&str1, 80);
	g_assert(addr.addr_up.raw);

	g_assert_cmpuint(AF_INET, ==, addr.addr_up.plain->sa_family);
	ipv4 = addr.addr_up.ipv4;

	g_assert_cmpuint(ipv4->sin_addr.s_addr, ==, htonl(0x7f000001u));

	g_assert(li_ipv4_in_ipv4_net(ipv4->sin_addr.s_addr, range.ipv4.addr, range.ipv4.networkmask));
	g_assert_cmpuint(ipv4->sin_port, ==, htons(range.ipv4.port));

	li_sockaddr_clear(&addr);
}

static void test_ipv6_net1(void) {
	netrange range;
	liSocketAddress addr;
	const char str0[] = "[::/0]:80";
	GString str1 = li_const_gstring(CONST_STR_LEN("::1"));
	struct sockaddr_in6 *ipv6;

	g_assert(!li_parse_ipv4(str0, &range.ipv4.addr, &range.ipv4.networkmask, &range.ipv4.port));
	g_assert(li_parse_ipv6(str0, range.ipv6.addr, &range.ipv6.network, &range.ipv6.port));

	g_assert_cmpuint(range.ipv6.network, ==, 0);
	g_assert_cmpuint(range.ipv6.port, ==, 80);

	addr = li_sockaddr_from_string(&str1, 80);
	g_assert(addr.addr_up.raw);

	g_assert_cmpuint(AF_INET6, ==, addr.addr_up.plain->sa_family);
	ipv6 = addr.addr_up.ipv6;

	g_assert(li_ipv6_in_ipv6_net(ipv6->sin6_addr.s6_addr, range.ipv6.addr, range.ipv6.network));
	g_assert_cmpuint(ipv6->sin6_port, ==, htons(range.ipv6.port));

	li_sockaddr_clear(&addr);
}

int main(int argc, char **argv) {
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/ip-parser/test-localhost-in-all-ipv4-net", test_ipv4_net1);
	g_test_add_func("/ip-parser/test-localhost-in-all-ipv6-net", test_ipv6_net1);

	return g_test_run();
}


#include <lighttpd/radix.h>

static void test_radix_insert_lookup(void) {
	static const guint magic1 = 4235;
	guint value;

	/* 127.0.0.1 */
	in_addr_t ip1 = htonl(INADDR_LOOPBACK);
	liRadixTree *rd = li_radixtree_new();

	li_radixtree_insert(rd, &ip1, 32, GUINT_TO_POINTER(magic1));

	value = GPOINTER_TO_UINT(li_radixtree_lookup_exact(rd, &ip1, 32));

	g_assert_cmpuint(magic1, ==, value);

	li_radixtree_free(rd, NULL, NULL);
}

static void test_radix_insert_insert_lookup(void) {
	static const guint magic1 = 4235 /* 0x108b */, magic2 = 59234 /* 0xe762 */;
	guint value;

	/* 127.0.0.1, 192.168.0.125 */
	/* as guint32: 2130706433, 3232235645 */
	in_addr_t ip1 = htonl(INADDR_LOOPBACK), ip2 = htonl( (in_addr_t) 0xC0A8007D );
	liRadixTree *rd = li_radixtree_new();

	li_radixtree_insert(rd, &ip1, 32, GUINT_TO_POINTER(magic1));
	li_radixtree_insert(rd, &ip2, 32, GUINT_TO_POINTER(magic2));

	value = GPOINTER_TO_UINT(li_radixtree_lookup_exact(rd, &ip1, 32));

	g_assert_cmpuint(magic1, ==, value);

	value = GPOINTER_TO_UINT(li_radixtree_lookup_exact(rd, &ip2, 32));

	g_assert_cmpuint(magic2, ==, value);

	li_radixtree_free(rd, NULL, NULL);
}

static void test_radix_insert_insert_del_lookup(void) {
	static const guint magic1 = 4235 /* 0x108b */, magic2 = 59234 /* 0xe762 */;
	guint value;

	/* 127.0.0.1, 192.168.0.125 */
	/* as guint32: 2130706433, 3232235645 */
	in_addr_t ip1 = htonl(INADDR_LOOPBACK), ip2 = htonl( (in_addr_t) 0xC0A8007D );
	liRadixTree *rd = li_radixtree_new();

	li_radixtree_insert(rd, &ip1, 32, GUINT_TO_POINTER(magic1));
	li_radixtree_insert(rd, &ip2, 32, GUINT_TO_POINTER(magic2));
	li_radixtree_remove(rd, &ip2, 32);

	value = GPOINTER_TO_UINT(li_radixtree_lookup_exact(rd, &ip1, 32));

	g_assert_cmpuint(magic1, ==, value);

	li_radixtree_free(rd, NULL, NULL);
}

int main(int argc, char **argv) {
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/radix/insert-lookup", test_radix_insert_lookup);
	g_test_add_func("/radix/insert-insert-lookup", test_radix_insert_insert_lookup);
	g_test_add_func("/radix/insert-insert-del-lookup", test_radix_insert_insert_del_lookup);

	return g_test_run();
}

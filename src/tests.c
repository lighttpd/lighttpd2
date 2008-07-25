#include <stdio.h>


#include "base.h"
#include "log.h"

#include "http_request_parser.h"
#include "config_parser.h"

int request_test() {
	chunkqueue *cq;
	request *req;
	http_request_ctx ctx;
	handler_t res;

	cq = chunkqueue_new();
	req = request_new();

	http_request_parser_init(&ctx, req, cq);

	chunkqueue_append_mem(cq, CONST_STR_LEN(
		"GET / HTTP/1.1\r\n"
		"\r\n"
	));

	res = http_request_parse(NULL, NULL, &ctx);
	if (res != HANDLER_GO_ON) {
		fprintf(stderr, "Parser return %i", res);
		return -1;
	}

	return 0;
}

int main() {
	server *srv;
	GTimeVal start, end;
	gboolean result;

	guint32 ip, netmask;
	assert(parse_ipv4("10.0.3.8/24", &ip, &netmask));
	printf("parsed ip: %s\n", inet_ntoa(*(struct in_addr*) &ip));
	printf("parsed netmask: %s\n", inet_ntoa(*(struct in_addr*) &netmask));

	guint8 ipv6[16];
	guint network;
	GString *s;
	assert(parse_ipv6("::ffff:192.168.0.1/80", ipv6, &network));
	s = ipv6_tostring(ipv6);
	printf("parsed ipv6: %s/%u\n", s->str, network);

	return 0;

	srv = server_new();

	/* config parser test */
	GList *cpd_stack = NULL;
	g_get_current_time(&start);
	result = config_parser_file(srv, &cpd_stack, "../test.conf");
	g_get_current_time(&end);

	printf("parsed config in %ld seconds %ld milliseconds and %ld microseconds\n",
		end.tv_sec - start.tv_sec,
		(end.tv_usec - start.tv_usec) / 1000,
		(end.tv_usec - start.tv_usec) % 1000
	);

	return request_test();
}

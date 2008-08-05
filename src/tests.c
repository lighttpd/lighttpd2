#include <stdio.h>


#include "base.h"
#include "log.h"

#include "http_request_parser.h"
#include "config_parser.h"

int request_test() {
	chunkqueue *cq;
	request req;
	handler_t res;

	cq = chunkqueue_new();
	request_init(&req, cq);

	chunkqueue_append_mem(cq, CONST_STR_LEN(
		"GET / HTTP/1.1\r\n"
		"Host: www.example.com\r\n"
		"\r\n"
		"abc"
	));

	res = http_request_parse(NULL, NULL, &req.parser_ctx);
	if (res != HANDLER_GO_ON) {
		fprintf(stderr, "Parser return %i", res);
	}

	assert(req.http_method == HTTP_METHOD_GET);
	assert(cq->length == 3);

	request_clear(&req);
	chunkqueue_free(cq);

	return res == HANDLER_GO_ON ? 0 : 1;
}

int main() {
	server *srv;

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

	request_test();

	return 0;

	srv = server_new();
	return 0;
}

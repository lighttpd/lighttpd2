
#include <lighttpd/base.h>

#define perror(msg) g_error("(%s:%i) %s failed: %s", __FILE__, __LINE__, msg, g_strerror(errno))

static void test_send_fd(void) {
	int pipefds[2], sockfd[2], rfd = -1;
	char buf[6];

	g_printerr("start\n");

	if (-1 == pipe(pipefds)) {
		perror("pipe");
	}

	if (-1 == socketpair(AF_UNIX, SOCK_STREAM, 0, sockfd)) {
		perror("socketpair");
	}

	if (-1 == li_send_fd(sockfd[0], pipefds[0])) {
		perror("li_send_fd");
	}

	if (-1 == li_receive_fd(sockfd[1], &rfd)) {
		perror("li_receive_fd");
	}

	write(pipefds[1], CONST_STR_LEN("test\0"));

	if (-1 == read(rfd, buf, 5)) {
		perror("read");
	}

	buf[5] = '\0';
	g_test_message("received: %s", buf);

	g_assert_cmpstr(buf, ==, "test");

	close(pipefds[0]); close(pipefds[1]);
	close(sockfd[0]); close(sockfd[1]);
	close(rfd);
}


int main(int argc, char **argv) {
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/utils/send_fd", test_send_fd);

	return g_test_run();
}

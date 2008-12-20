
#include <lighttpd/base.h>
#include <lighttpd/angel.h>

/* listen to a socket */
int angel_listen(server *srv, GString *str) {
	return angel_fake_listen(srv, str);
}

/* send log messages while startup to angel */
gboolean angel_log(server *srv, GString *str) {
	return angel_fake_log(srv, str);
}

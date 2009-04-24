
#include <lighttpd/angel_base.h>

server* server_new(const gchar *module_dir) {
	server *srv = g_slice_new0(server);

	/* TODO: handle sinals */

	srv->loop = ev_default_loop(0);
	log_init(srv);
	plugins_init(srv, module_dir);
	return srv;
}

void server_free(server* srv) {
	plugins_clear(srv);

	log_clean(srv);
	g_slice_free(server, srv);
}

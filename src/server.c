
#include "base.h"


server* server_new() {
	server* srv = g_slice_new0(server);
	srv->plugins = g_hash_table_new(g_str_hash, g_str_equal);
	srv->options = g_hash_table_new(g_str_hash, g_str_equal);
	srv->mutex = g_mutex_new();
	srv->error_log_fd = STDERR_FILENO;

	return srv;
}

void server_free(server* srv) {
	if (!srv) return;
	/* TODO */

	g_hash_table_destroy(srv->plugins);
	g_hash_table_destroy(srv->options);
	g_mutex_free(srv->mutex);

	g_slice_free(server, srv);
}

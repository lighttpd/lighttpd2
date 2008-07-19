
#include "base.h"
#include "log.h"


server* server_new() {
	server* srv = g_slice_new0(server);
	srv->plugins = g_hash_table_new(g_str_hash, g_str_equal);
	srv->options = g_hash_table_new(g_str_hash, g_str_equal);
	srv->mutex = g_mutex_new();

	return srv;
}

void server_free(server* srv) {
	if (!srv) return;
	/* TODO */

	g_hash_table_destroy(srv->plugins);
	g_hash_table_destroy(srv->options);
	g_mutex_free(srv->mutex);

	/* free logs */
	g_hash_table_destroy(srv->logs);

	g_async_queue_unref(srv->log_queue);

	g_slice_free(server, srv);
}

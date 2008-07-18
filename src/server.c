
#include "base.h"
#include "log.h"


server* server_new() {
	server* srv = g_slice_new0(server);
	srv->plugins = g_hash_table_new(g_str_hash, g_str_equal);
	srv->options = g_hash_table_new(g_str_hash, g_str_equal);
	srv->mutex = g_mutex_new();

	srv->logs = g_array_new(FALSE, FALSE, sizeof(log_t));
	srv->log_queue = g_async_queue_new();
	srv->exiting = FALSE;

	return srv;
}

void server_free(server* srv) {
	if (!srv) return;
	/* TODO */

	g_hash_table_destroy(srv->plugins);
	g_hash_table_destroy(srv->options);
	g_mutex_free(srv->mutex);

	/* free logs */
	for (guint i; i < srv->logs->len; i++) {
		log_t *log = &g_array_index(srv->logs, log_t, i);
		log_free(log);
	}
	g_array_free(srv->logs, TRUE);

	g_async_queue_unref(srv->log_queue);

	g_slice_free(server, srv);
}

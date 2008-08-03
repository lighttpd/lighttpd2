
#include "base.h"
#include "log.h"

static void server_option_free(gpointer _so) {
	g_slice_free(server_option, _so);
}

static void server_action_free(gpointer _sa) {
	g_slice_free(server_action, _sa);
}

static void server_setup_free(gpointer _ss) {
	g_slice_free(server_setup, _ss);
}

server* server_new() {
	server* srv = g_slice_new0(server);
	srv->plugins = g_hash_table_new(g_str_hash, g_str_equal);
	srv->options = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, server_option_free);
	srv->actions = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, server_action_free);
	srv->setups  = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, server_setup_free);
	srv->mutex = g_mutex_new();
	srv->mainactionlist = action_list_new();

	return srv;
}

void server_free(server* srv) {
	if (!srv) return;
	/* TODO */

	g_hash_table_destroy(srv->options);
	g_hash_table_destroy(srv->actions);
	g_hash_table_destroy(srv->setups);
	g_hash_table_destroy(srv->plugins);
	g_mutex_free(srv->mutex);

	action_list_release(srv, srv->mainactionlist);

	/* free logs */
	GHashTableIter iter;
	gpointer k, v;
	g_hash_table_iter_init(&iter, srv->logs);
	while (g_hash_table_iter_next(&iter, &k, &v)) {
		log_free(srv, v);
	}
	g_hash_table_destroy(srv->logs);

	g_mutex_free(srv->log_mutex);
	g_async_queue_unref(srv->log_queue);

	g_slice_free(server, srv);
}



struct server {
	guint version;

	GHashTable *plugins;

	size_t option_count;
	GHashTable *options;
	gpointer *option_def_values;

	gboolean exiting;
	GMutex *mutex;

	/* logs */
	gboolean rotate_logs;
	GHashTable *logs;
	struct log_t *log_stderr;
	struct log_t *log_syslog;
	GAsyncQueue *log_queue;
	GThread *log_thread;
	GMutex *log_mutex;
};



server* server_new();
void server_free(server* srv);

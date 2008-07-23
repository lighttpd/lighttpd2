#ifndef _LIGHTTPD_SERVER_H_
#define _LIGHTTPD_SERVER_H_


struct server {
	guint version;

	GHashTable *plugins;

	struct action_list *actions;

	size_t option_count;
	GHashTable *options;
	gpointer *option_def_values;

	gboolean exiting;
	GMutex *mutex; /* general mutex for accessing the various members */

	/* logs */
	gboolean rotate_logs;
	GHashTable *logs;
	struct log_t *log_stderr;
	struct log_t *log_syslog;
	GAsyncQueue *log_queue;
	GThread *log_thread;
	GMutex *log_mutex; /* manage access for the logs hashtable */
};



server* server_new();
void server_free(server* srv);

#endif

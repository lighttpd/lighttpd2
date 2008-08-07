#ifndef _LIGHTTPD_SERVER_H_
#define _LIGHTTPD_SERVER_H_

#ifndef LIGHTTPD_SERVER_MAGIC
#define LIGHTTPD_SERVER_MAGIC ((guint)0x12AB34CD)
#endif

typedef enum {
	SERVER_STARTING,         /** start up: don't write log files, don't accept connections */
	SERVER_RUNNING,          /** running: write logs, accept connections */
	SERVER_STOPPING          /** stopping: flush logs, don't accept new connections */
} server_state;

struct server_socket;
typedef struct server_socket server_socket;

struct server_socket {
	server *srv;
	ev_io watcher;
};

struct server {
	guint32 magic;            /** server magic version, check against LIGHTTPD_SERVER_MAGIC in plugins */
	server_state state;

	struct ev_loop *loop;

	guint connections_active; /** 0..con_act-1: active connections, con_act..used-1: free connections */
	GArray *connections;      /** array of (connection*) */
	GArray *sockets;          /** array of (server_socket*) */

	GHashTable *plugins;      /**< const gchar* => (plugin*) */

	/* registered by plugins */
	GHashTable *options;      /**< const gchar* => (server_option*) */
	GHashTable *actions;      /**< const gchar* => (server_action*) */
	GHashTable *setups;       /**< const gchar* => (server_setup*) */

	gpointer *option_def_values; /* TODO */
	struct action *mainaction;

	gboolean exiting;

	GString *tmp_str;         /**< can be used everywhere for local temporary needed strings */

	time_t cur_ts, last_generated_date_ts;
	GString *ts_date_str;     /**< use server_current_timestamp(srv) */

	/* logs */
	gboolean rotate_logs;
	GHashTable *logs;
	struct log_t *log_stderr;
	struct log_t *log_syslog;
	GAsyncQueue *log_queue;
	GThread *log_thread;
	GMutex *log_mutex; /* manage access for the logs hashtable */
};


LI_API server* server_new();
LI_API void server_free(server* srv);

LI_API void server_listen(server *srv, int fd);

LI_API void server_start(server *srv);
LI_API void server_stop(server *srv);

LI_API void joblist_append(server *srv, connection *con);

LI_API GString *server_current_timestamp(server *srv);

#endif

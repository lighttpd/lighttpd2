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

struct statistics_t;
typedef struct statistics_t statistics_t;

struct statistics_t {
	guint64 bytes_out;        /** bytes transfered, outgoing */
	guint64 bytes_int;        /** bytes transfered, incoming */

	guint64 requests;         /** processed requests */

	guint64 actions_executed; /** actions executed */
};

struct worker;

struct server {
	guint32 magic;            /** server magic version, check against LIGHTTPD_SERVER_MAGIC in plugins */
	server_state state;       /** atomic access */

	struct worker *main_worker;
	guint worker_count;
	GArray *workers;

	guint loop_flags;
	ev_signal
		sig_w_INT,
		sig_w_TERM,
		sig_w_PIPE;
	ev_prepare srv_prepare;
	ev_check srv_check;

	GArray *sockets;          /** array of (server_socket*) */

	GHashTable *plugins;      /**< const gchar* => (plugin*) */

	/* registered by plugins */
	GHashTable *options;      /**< const gchar* => (server_option*) */
	GHashTable *actions;      /**< const gchar* => (server_action*) */
	GHashTable *setups;       /**< const gchar* => (server_setup*) */

	GArray *plugins_handle_close; /** list of handle_close callbacks */

	size_t option_count;      /**< set to size of option hash table */
	gpointer *option_def_values;
	struct action *mainaction;

	gboolean exiting;         /** atomic access */

	struct {
		GMutex *mutex;
		GHashTable *targets;  /** const gchar* path => (log_t*) */
		GAsyncQueue *queue;
		GThread *thread;
		gboolean stop_thread; /** access with atomic functions */
		GArray *timestamps;   /** array of log_timestamp_t */
		struct log_t *stderr;
	} logs;

	ev_tstamp started;

	/* keep alive timeout */
	guint keep_alive_queue_timeout;
};


LI_API server* server_new();
LI_API void server_free(server* srv);
LI_API gboolean server_loop_init(server *srv);

LI_API void server_listen(server *srv, int fd);

/* Start accepting connection, use log files, no new plugins after that */
LI_API void server_start(server *srv);
/* stop accepting connections, turn keep-alive off, close all shutdown sockets, set exiting = TRUE */
LI_API void server_stop(server *srv);
/* exit asap with cleanup */
LI_API void server_exit(server *srv);

LI_API void joblist_append(connection *con);

LI_API GString *server_current_timestamp();

#endif

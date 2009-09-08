#ifndef _LIGHTTPD_SERVER_H_
#define _LIGHTTPD_SERVER_H_

#ifndef LIGHTTPD_SERVER_MAGIC
#define LIGHTTPD_SERVER_MAGIC ((guint)0x12AB34CD)
#endif

typedef gboolean (*liConnectionNewCB)(liConnection *con);
typedef void (*liConnectionCloseCB)(liConnection *con);
typedef liNetworkStatus (*liConnectionWriteCB)(liConnection *con, goffset write_max);
typedef liNetworkStatus (*liConnectionReadCB)(liConnection *con);
typedef void (*liServerSocketReleaseCB)(liServerSocket *srv_sock);

typedef enum {
	LI_SERVER_INIT,             /** start state */
	LI_SERVER_LOADING,          /** config loaded, prepare listeing sockets/open log files */
	LI_SERVER_SUSPENDED,        /** ready to go, no logs */
	LI_SERVER_WARMUP,           /** listen() active, no logs yet, handling remaining connections */
	LI_SERVER_RUNNING,          /** listen() and logs active */
	LI_SERVER_SUSPENDING,       /** listen() stopped, logs active, handling remaining connections */
	LI_SERVER_STOPPING,         /** listen() stopped, no logs, handling remaining connections */
	LI_SERVER_DOWN              /** exit */
} liServerState;

struct liServerSocket {
	gint refcount;
	liServer *srv;
	ev_io watcher;
	liSocketAddress local_addr;
	GString *local_addr_str;

	/* Custom sockets (ssl) */
	gpointer data;
	liConnectionWriteCB write_cb;
	liConnectionReadCB read_cb;
	liConnectionNewCB new_cb;
	liConnectionCloseCB close_cb;
	liServerSocketReleaseCB release_cb;
};

struct liServer {
	guint32 magic;            /** server magic version, check against LIGHTTPD_SERVER_MAGIC in plugins */
	liServerState state, dest_state;       /** atomic access */
	liAngelConnection *acon;

	liWorker *main_worker;
	guint worker_count;
	GArray *workers;
	GArray *ts_formats;      /** array of (GString*), add with li_server_ts_format_add() */

	struct ev_loop *loop;
	guint loop_flags;
	ev_signal
		sig_w_INT,
		sig_w_TERM,
		sig_w_PIPE;
	ev_prepare srv_prepare;
	ev_check srv_check;

	GPtrArray *sockets;          /** array of (server_socket*) */

	liModules *modules;

	GHashTable *plugins;      /**< const gchar* => (liPlugin*) */
	liPlugin *core_plugin;

	/* registered by plugins */
	GHashTable *options;      /**< const gchar* => (liServerOption*) */
	GHashTable *actions;      /**< const gchar* => (liServerAction*) */
	GHashTable *setups;       /**< const gchar* => (liServerSetup*) */

	GArray *li_plugins_handle_close; /** list of handle_close callbacks */
	GArray *li_plugins_handle_vrclose; /** list of handle_vrclose callbacks */

	GArray *option_def_values;/** array of option_value */
	liAction *mainaction;

	gboolean exiting;         /** atomic access */

	struct {
		GMutex *mutex;
		GHashTable *targets;    /** const gchar* path => (log_t*) */
		GAsyncQueue *queue;
		GThread *thread;
		gboolean thread_finish; /** finish writing logs in the queue, then exit thread; access with atomic functions */
		gboolean thread_stop;   /** stop thread immediately; access with atomic functions */
		gboolean thread_alive;  /** access with atomic functions */
		GArray *timestamps;     /** array of log_timestamp_t */
		liLog *stderr;
	} logs;

	ev_tstamp started;
	GString *started_str;

	/* keep alive timeout */
	guint keep_alive_queue_timeout;

	gdouble io_timeout;

	GArray *throttle_pools;

	gdouble stat_cache_ttl;
};


LI_API liServer* li_server_new(const gchar *module_dir);
LI_API void li_server_free(liServer* srv);
LI_API gboolean li_server_loop_init(liServer *srv);
LI_API gboolean li_server_worker_init(liServer *srv);

LI_API liServerSocket* li_server_listen(liServer *srv, int fd);

/* exit asap with cleanup */
LI_API void li_server_exit(liServer *srv);

LI_API GString *li_server_current_timestamp();

LI_API void li_server_out_of_fds(liServer *srv);

LI_API guint li_server_ts_format_add(liServer *srv, GString* format);

LI_API void li_server_socket_release(liServerSocket* sock);
LI_API void li_server_socket_acquire(liServerSocket* sock);

LI_API void li_server_goto_state(liServer *srv, liServerState state);
LI_API void li_server_reached_state(liServer *srv, liServerState state);

#endif

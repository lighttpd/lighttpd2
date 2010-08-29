#ifndef _LIGHTTPD_SERVER_H_
#define _LIGHTTPD_SERVER_H_

#ifndef _LIGHTTPD_BASE_H_
#error Please include <lighttpd/base.h> instead of this file
#endif

struct lua_State;

#ifndef LIGHTTPD_SERVER_MAGIC
# define LIGHTTPD_SERVER_MAGIC ((guint)0x12AB34CD)
#endif

typedef gboolean (*liConnectionNewCB)(liConnection *con);
typedef void (*liConnectionCloseCB)(liConnection *con);
typedef liNetworkStatus (*liConnectionWriteCB)(liConnection *con, goffset write_max);
typedef liNetworkStatus (*liConnectionReadCB)(liConnection *con);
typedef void (*liServerSocketReleaseCB)(liServerSocket *srv_sock);

typedef void (*liServerStateWaitCancelled)(liServer *srv, liServerStateWait *w);

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

	/* Custom sockets (ssl) */
	gpointer data;
	liConnectionWriteCB write_cb;
	liConnectionReadCB read_cb;
	liConnectionNewCB new_cb;
	liConnectionCloseCB close_cb;
	liServerSocketReleaseCB release_cb;
};

struct liServerStateWait {
	GList queue_link;
	gboolean active;
	liServerStateWaitCancelled cancel_cb;
	gpointer data;
};

struct liServer {
	guint32 magic;            /** server magic version, check against LIGHTTPD_SERVER_MAGIC in plugins */
	liServerState state, dest_state;       /** atomic access */
	liAngelConnection *acon;

	/* state machine handling */
	GMutex *statelock;
	GQueue state_wait_queue;
	liServerState state_wait_for;
	ev_async state_ready_watcher;

	GMutex *lualock;
	struct lua_State *L;     /** NULL if compiled without Lua */

	liWorker *main_worker;
	guint worker_count;
	GArray *workers;
#ifdef LIGHTY_OS_LINUX
	liValue *workers_cpu_affinity;
#endif
	GArray *ts_formats;      /** array of (GString*), add with li_server_ts_format_add() */

	struct ev_loop *loop;
	guint loop_flags;
	ev_signal
		sig_w_INT,
		sig_w_TERM,
		sig_w_PIPE;
	ev_timer srv_1sec_timer;

	GPtrArray *sockets;          /** array of (server_socket*) */

	liModules *modules;

	GHashTable *plugins;      /**< const gchar* => (liPlugin*) */
	liPlugin *core_plugin;

	/* registered by plugins */
	GHashTable *options;      /**< const gchar* => (liServerOption*) */
	GHashTable *optionptrs;   /**< const gchar* => (liServerOptionPtr*) */
	GHashTable *actions;      /**< const gchar* => (liServerAction*) */
	GHashTable *setups;       /**< const gchar* => (liServerSetup*) */

	GArray *li_plugins_handle_close; /** list of handle_close callbacks */
	GArray *li_plugins_handle_vrclose; /** list of handle_vrclose callbacks */

	GArray *option_def_values;/** array of liOptionValue */
	GArray *optionptr_def_values;/** array of liOptionPtrValue* */
	liAction *mainaction;

	gboolean exiting;         /** atomic access */

	struct {
		struct ev_loop *loop;
		ev_async watcher;
		liRadixTree *targets;    /** const gchar* path => (liLog*) */
		liWaitQueue close_queue;
		GQueue write_queue;
		GStaticMutex write_queue_mutex;
		GThread *thread;
		gboolean thread_alive;
		gboolean thread_finish;
		gboolean thread_stop;
		GArray *timestamps;
	} logs;

	ev_tstamp started;
	GString *started_str;

	guint connection_load, max_connections;
	gboolean connection_limit_hit; /** true if limit was hit and the sockets are disabled */

	/* keep alive timeout */
	guint keep_alive_queue_timeout;

	gdouble io_timeout;

	GArray *throttle_pools;

	gdouble stat_cache_ttl;
	gint tasklet_pool_threads;
};


LI_API liServer* li_server_new(const gchar *module_dir, gboolean module_resident);
LI_API void li_server_free(liServer* srv);
LI_API gboolean li_server_loop_init(liServer *srv);

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

/** threadsafe */
LI_API void li_server_state_ready(liServer *srv, liServerStateWait *sw);

/** only call from server state plugin hooks; push new wait condition to wait queue */
LI_API void li_server_state_wait(liServer *srv, liServerStateWait *sw);

#endif

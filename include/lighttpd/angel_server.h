#ifndef _LIGHTTPD_ANGEL_SERVER_H_
#define _LIGHTTPD_ANGEL_SERVER_H_

#ifndef _LIGHTTPD_ANGEL_BASE_H_
#error Please include <lighttpd/angel_base.h> instead of this file
#endif

#ifndef LIGHTTPD_ANGEL_MAGIC
#define LIGHTTPD_ANGEL_MAGIC ((guint)0x3e14ac65)
#endif

typedef enum {
	INSTANCE_DOWN,    /* not running */
	INSTANCE_LOADING, /* startup */
	INSTANCE_WARMUP,  /* running, but logging to files disabled */
	INSTANCE_ACTIVE,  /* everything running */
	INSTANCE_SUSPEND  /* handle remaining connections, suspend logs+accept() */
} instance_state_t;

struct instance_conf {
	gint refcount;

	gchar **cmd;
	GString *username;
	uid_t uid;
	gid_t gid;
};

struct instance {
	gint refcount;

	server *srv;
	instance_conf *ic;

	pid_t pid;
	ev_child child_watcher;

	instance_state_t s_cur, s_dest;

	instance *replace, *replace_by;

	angel_connection *acon;
	gboolean in_jobqueue;
};

struct server {
	guint32 magic;            /** server magic version, check against LIGHTTPD_ANGEL_MAGIC in plugins */

	struct ev_loop *loop;
	ev_signal
		sig_w_INT,
		sig_w_TERM,
		sig_w_PIPE;

	GQueue job_queue;
	ev_async job_watcher;

	Plugins plugins;

	log_t log;
};

LI_API server* server_new(const gchar *module_dir);
LI_API void server_free(server* srv);

LI_API instance* server_new_instance(server *srv, instance_conf *ic);
LI_API void instance_replace(instance *oldi, instance *newi);
LI_API void instance_set_state(instance *i, instance_state_t s);

LI_API instance_conf* instance_conf_new(gchar **cmd, GString *username, uid_t uid, gid_t gid);
LI_API void instance_conf_release(instance_conf *ic);
LI_API void instance_conf_acquire(instance_conf *ic);

LI_API void instance_release(instance *i);
LI_API void instance_acquire(instance *i);

LI_API void instance_job_append(instance *i);

#endif

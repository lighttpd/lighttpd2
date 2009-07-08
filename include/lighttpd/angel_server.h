#ifndef _LIGHTTPD_ANGEL_SERVER_H_
#define _LIGHTTPD_ANGEL_SERVER_H_

#ifndef _LIGHTTPD_ANGEL_BASE_H_
#error Please include <lighttpd/angel_base.h> instead of this file
#endif

#ifndef LIGHTTPD_ANGEL_MAGIC
#define LIGHTTPD_ANGEL_MAGIC ((guint)0x3e14ac65)
#endif

typedef enum {
	LI_INSTANCE_DOWN,    /* not running */
	LI_INSTANCE_LOADING, /* startup */
	LI_INSTANCE_WARMUP,  /* running, but logging to files disabled */
	LI_INSTANCE_ACTIVE,  /* everything running */
	LI_INSTANCE_SUSPEND  /* handle remaining connections, suspend logs+accept() */
} liInstanceState;

struct liInstanceConf {
	gint refcount;

	gchar **cmd;
	GString *username;
	uid_t uid;
	gid_t gid;
};

struct liInstance {
	gint refcount;

	liServer *srv;
	liInstanceConf *ic;

	pid_t pid;
	ev_child child_watcher;

	liInstanceState s_cur, s_dest;

	liInstance *replace, *replace_by;

	liAngelConnection *acon;
	gboolean in_jobqueue;
};

struct liServer {
	guint32 magic;            /** server magic version, check against LIGHTTPD_ANGEL_MAGIC in plugins */

	struct ev_loop *loop;
	ev_signal
		sig_w_INT,
		sig_w_TERM,
		sig_w_PIPE;

	GQueue job_queue;
	ev_async job_watcher;

	liPlugins plugins;

	liLog log;
};

LI_API liServer* server_new(const gchar *module_dir);
LI_API void server_free(liServer* srv);

LI_API liInstance* server_new_instance(liServer *srv, liInstanceConf *ic);
LI_API void instance_replace(liInstance *oldi, liInstance *newi);
LI_API void instance_set_state(liInstance *i, liInstanceState s);

LI_API liInstanceConf* instance_conf_new(gchar **cmd, GString *username, uid_t uid, gid_t gid);
LI_API void instance_conf_release(liInstanceConf *ic);
LI_API void instance_conf_acquire(liInstanceConf *ic);

LI_API void instance_release(liInstance *i);
LI_API void instance_acquire(liInstance *i);

LI_API void instance_job_append(liInstance *i);

#endif

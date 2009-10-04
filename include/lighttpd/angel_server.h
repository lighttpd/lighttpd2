#ifndef _LIGHTTPD_ANGEL_SERVER_H_
#define _LIGHTTPD_ANGEL_SERVER_H_

#ifndef _LIGHTTPD_ANGEL_BASE_H_
#error Please include <lighttpd/angel_base.h> instead of this file
#endif

#ifndef LIGHTTPD_ANGEL_MAGIC
#define LIGHTTPD_ANGEL_MAGIC ((guint)0x3e14ac65)
#endif

typedef enum {
	LI_INSTANCE_DOWN,       /* not started yet */
	LI_INSTANCE_SUSPENDED,  /* inactive, neither accept nor logs, handle remaining connections */
	LI_INSTANCE_WARMUP,     /* only accept(), no logging: waiting for another instance to suspend */
	LI_INSTANCE_RUNNING,    /* everything running */
	LI_INSTANCE_SUSPENDING, /* suspended accept(), still logging, handle remaining connections  */
	LI_INSTANCE_FINISHED    /* not running */
} liInstanceState;

struct liInstanceConf {
	gint refcount;

	gchar **cmd;
	gchar **env;
	GString *username;
	uid_t uid;
	gid_t gid;

	gint64 rlim_core, rlim_nofile; /* < 0: don't change, G_MAXINT64: unlimited */
};

struct liInstance {
	gint refcount;

	liServer *srv;
	liInstanceConf *ic;

	liProc *proc;
	ev_child child_watcher;

	liInstanceState s_cur, s_dest;

	liInstance *replace, *replace_by;

	liAngelConnection *acon;
};

struct liServer {
	guint32 magic;            /** server magic version, check against LIGHTTPD_ANGEL_MAGIC in plugins */

	struct ev_loop *loop;
	ev_signal
		sig_w_INT,
		sig_w_TERM,
		sig_w_PIPE;

	liPlugins plugins;

	liLog log;
};

LI_API liServer* li_server_new(const gchar *module_dir);
LI_API void li_server_free(liServer* srv);

LI_API void li_server_stop(liServer *srv);

LI_API liInstance* li_server_new_instance(liServer *srv, liInstanceConf *ic);
LI_API void li_instance_replace(liInstance *oldi, liInstance *newi);
LI_API void li_instance_set_state(liInstance *i, liInstanceState s);
LI_API void li_instance_state_reached(liInstance *i, liInstanceState s);

LI_API liInstanceConf* li_instance_conf_new(gchar **cmd, gchar **env, GString *username, uid_t uid, gid_t gid, gint64 rlim_core, gint64 rlim_nofile);
LI_API void li_instance_conf_release(liInstanceConf *ic);
LI_API void li_instance_conf_acquire(liInstanceConf *ic);

LI_API void li_instance_release(liInstance *i);
LI_API void li_instance_acquire(liInstance *i);

#endif

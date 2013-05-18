#ifndef _LIGHTTPD_ANGEL_SERVER_H_
#define _LIGHTTPD_ANGEL_SERVER_H_

#ifndef _LIGHTTPD_ANGEL_BASE_H_
#error Please include <lighttpd/angel_base.h> instead of this file
#endif

#ifndef LIGHTTPD_ANGEL_MAGIC
#define LIGHTTPD_ANGEL_MAGIC ((guint)0x3e14ac65)
#endif

typedef void (*liInstanceResourceFreeCB)    (liServer *srv, liInstance *i, liPlugin *p, liInstanceResource *res);

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
	liEventChild child_watcher;

	liInstanceState s_cur, s_dest;

	liInstance *replace, *replace_by;

	liAngelConnection *acon;

	GPtrArray *resources;
};

struct liServer {
	guint32 magic;            /** server magic version, check against LIGHTTPD_ANGEL_MAGIC in plugins */

	liEventLoop loop;
	liEventSignal
		sig_w_INT,
		sig_w_TERM,
		sig_w_PIPE;

	liPlugins plugins;

	liLog log;
};

struct liInstanceResource {
	liInstanceResourceFreeCB free_cb;
	liPlugin *plugin; /* may be NULL - we don't care about that */
	guint ndx; /* internal array index */

	gpointer data;
};

LI_API liServer* li_server_new(const gchar *module_dir, gboolean module_resident);
LI_API void li_server_free(liServer* srv);

LI_API void li_server_stop(liServer *srv);

LI_API liInstance* li_server_new_instance(liServer *srv, liInstanceConf *ic);
LI_API gboolean li_instance_replace(liInstance *oldi, liInstance *newi);
LI_API void li_instance_set_state(liInstance *i, liInstanceState s);
LI_API void li_instance_state_reached(liInstance *i, liInstanceState s);

LI_API liInstanceConf* li_instance_conf_new(gchar **cmd, gchar **env, GString *username, uid_t uid, gid_t gid, gint64 rlim_core, gint64 rlim_nofile);
LI_API void li_instance_conf_release(liInstanceConf *ic);
LI_API void li_instance_conf_acquire(liInstanceConf *ic);

LI_API void li_instance_release(liInstance *i);
LI_API void li_instance_acquire(liInstance *i);

LI_API void li_instance_add_resource(liInstance *i, liInstanceResource *res, liInstanceResourceFreeCB free_cb, liPlugin *p, gpointer data);
LI_API void li_instance_rem_resource(liInstance *i, liInstanceResource *res);

#endif

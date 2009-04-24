#ifndef _LIGHTTPD_ANGEL_SERVER_H_
#define _LIGHTTPD_ANGEL_SERVER_H_

#ifndef _LIGHTTPD_ANGEL_BASE_H_
#error Please include <lighttpd/angel_base.h> instead of this file
#endif

#ifndef LIGHTTPD_ANGEL_MAGIC
#define LIGHTTPD_ANGEL_MAGIC ((guint)0x3e14ac65)
#endif

struct instance {
	pid_t pid;
};

struct server {
	guint32 magic;            /** server magic version, check against LIGHTTPD_ANGEL_MAGIC in plugins */

	struct ev_loop *loop;
	ev_signal
		sig_w_INT,
		sig_w_TERM,
		sig_w_PIPE;

	Plugins plugins;

	log_t log;
};

LI_API server* server_new(const gchar *module_dir);
LI_API void server_free(server* srv);

#endif

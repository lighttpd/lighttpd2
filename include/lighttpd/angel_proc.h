#ifndef _LIGHTTPD_ANGEL_PROC_H_
#define _LIGHTTPD_ANGEL_PROC_H_

#ifndef _LIGHTTPD_ANGEL_BASE_H_
#error Please include <lighttpd/angel_base.h> instead of this file
#endif

/* The callback is not allowed to close the epipe */
typedef void (*liErrorPipeCB)(liServer *srv, liErrorPipe *epipe, GString *msg);

typedef void (*liProcSetupCB)(gpointer ctx);

struct liErrorPipe {
	liServer *srv;
	gpointer ctx;
	liErrorPipeCB cb;

	int fds[2];
	ev_io fd_watcher;
};

struct liProc {
	liServer *srv;

	pid_t child_pid;
	liErrorPipe *epipe;
	gchar *appname;
};

LI_API liErrorPipe* li_error_pipe_new(liServer *srv, liErrorPipeCB cb, gpointer ctx);
LI_API void li_error_pipe_free(liErrorPipe *epipe);

/** closes out-fd */
LI_API void li_error_pipe_activate(liErrorPipe *epipe);

/** closes in-fd, moves out-fd to dest_fd */
LI_API void li_error_pipe_use(liErrorPipe *epipe, int dest_fd);

/** read remaining data from in-fd */
LI_API void li_error_pipe_flush(liErrorPipe *epipe);

LI_API liProc* li_proc_new(liServer *srv, gchar **args, gchar **env, uid_t uid, gid_t gid, gchar *username, gint64 rlim_core, gint64 rlim_nofile, liProcSetupCB cb, gpointer ctx);
LI_API void li_proc_free(liProc *proc);

#endif

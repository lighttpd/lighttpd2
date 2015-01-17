
#include <lighttpd/angel_base.h>

#include <grp.h>

#ifdef HAVE_SYS_RESOURCE_H
# include <sys/resource.h>
#endif

static void read_pipe(liServer *srv, liErrorPipe *epipe, gboolean flush) {
	const ssize_t max_read = 8192;
	ssize_t r, toread = 0;
	GString *buf;
	int count = 10;

	if (-1 == epipe->fds[0]) return;

	for (;;) {
		if (ioctl(epipe->fds[0], FIONREAD, &toread) || toread == 0) {
			toread = 256;
		} else {
			if (toread < 0 || toread > max_read) toread = max_read;
		}

		buf = g_string_sized_new(toread);
		g_string_set_size(buf, toread);

		r = read(epipe->fds[0], buf->str, toread);
		if (r < 0) {
			g_string_free(buf, TRUE);
			switch (errno) {
			case EINTR: continue;
			case EAGAIN:
#if EWOULDBLOCK != EAGAIN
			case EWOULDBLOCK:
#endif
				return; /* come back later */
			case ECONNRESET:
				goto close_epipe;
			default:
				ERROR(srv, "read error: %s", g_strerror(errno));
				goto close_epipe;
			}
		} else if (r == 0) { /* EOF */
			g_string_free(buf, TRUE);
			goto close_epipe;
		}

		g_string_set_size(buf, r);
		epipe->cb(srv, epipe, buf);
		g_string_free(buf, TRUE);

		if (!flush) break;

		if (--count <= 0) {
			buf = g_string_new("error while trying to flush error-pipe: didn't see EOF. closing");
			epipe->cb(srv, epipe, buf);
			g_string_free(buf, TRUE);
			return;
		}
	}

	return;

close_epipe:
	li_event_stop(&epipe->fd_watcher);
	close(epipe->fds[0]);
	epipe->fds[0] = -1;
}

static void error_pipe_cb(liEventBase *watcher, int events) {
	liErrorPipe *epipe = LI_CONTAINER_OF(li_event_io_from(watcher), liErrorPipe, fd_watcher);
	UNUSED(events);

	read_pipe(epipe->srv, epipe, FALSE);
}

liErrorPipe* li_error_pipe_new(liServer *srv, liErrorPipeCB cb, gpointer ctx) {
	liErrorPipe *epipe;
	int fds[2];

	if (-1 == pipe(fds)) {
		ERROR(srv, "Couldn't create pipe: %s", g_strerror(errno));
		return NULL;
	}

	epipe = g_slice_new0(liErrorPipe);
	epipe->srv = srv;
	epipe->cb = cb;
	epipe->ctx = ctx;
	li_event_io_init(&srv->loop, "angel error-pipe", &epipe->fd_watcher, error_pipe_cb, fds[0], LI_EV_READ);
	epipe->fds[0] = fds[0];
	epipe->fds[1] = fds[1];

	li_fd_init(fds[0]);

	return epipe;
}

void li_error_pipe_free(liErrorPipe *epipe) {
	li_event_clear(&epipe->fd_watcher);
	li_error_pipe_flush(epipe);
	if (-1 != epipe->fds[0]) { close(epipe->fds[0]); epipe->fds[0] = -1; }
	if (-1 != epipe->fds[1]) { close(epipe->fds[1]); epipe->fds[1] = -1; }

	g_slice_free(liErrorPipe, epipe);
}

/** closes out-fd */
void li_error_pipe_activate(liErrorPipe *epipe) {
	if (-1 != epipe->fds[1]) { close(epipe->fds[1]); epipe->fds[1] = -1; }
	li_event_start(&epipe->fd_watcher);
}

/** closes in-fd, moves out-fd to dest_fd */
void li_error_pipe_use(liErrorPipe *epipe, int dest_fd) {
	if (-1 != epipe->fds[0]) {
		close(epipe->fds[0]);
		epipe->fds[0] = -1;
	}
	if (epipe->fds[1] != dest_fd) {
		dup2(epipe->fds[1], dest_fd);
		close(epipe->fds[1]);
		epipe->fds[1] = dest_fd;
	}
}

void li_error_pipe_flush(liErrorPipe *epipe) {
	read_pipe(epipe->srv, epipe, TRUE);
}

static void proc_epipe_cb(liServer *srv, liErrorPipe *epipe, GString *msg) {
	liProc *proc = epipe->ctx;

	BACKEND_LINES(srv, msg->str, "%s[%i]: ", proc->appname, proc->child_pid);
}

liProc* li_proc_new(liServer *srv, gchar **args, gchar **env, uid_t uid, gid_t gid, gchar *username, gint64 rlim_core, gint64 rlim_nofile, liProcSetupCB cb, gpointer ctx) {
	liProc *proc;
	pid_t pid;

	proc = g_slice_new0(liProc);
	proc->srv = srv;
	proc->child_pid = -1;
	proc->epipe = li_error_pipe_new(srv, proc_epipe_cb, proc);
	proc->appname = g_strdup(li_remove_path(args[0]));

	switch (pid = fork()) {
	case 0:
		li_error_pipe_use(proc->epipe, STDERR_FILENO);

		setsid();

#ifdef HAVE_GETRLIMIT
		{
			struct rlimit rlim;
			if (rlim_core >= 0) {
				rlim.rlim_cur = rlim.rlim_max = ((guint64) rlim_core >= RLIM_INFINITY) ? RLIM_INFINITY : (guint64) rlim_core;
				if (0 != setrlimit(RLIMIT_CORE, &rlim)) {
					ERROR(srv, "couldn't set 'max core file size': %s", g_strerror(errno));
				}
			}
			if (rlim_nofile >= 0) {
				rlim.rlim_cur = rlim.rlim_max = ((guint64) rlim_nofile >= RLIM_INFINITY) ? RLIM_INFINITY : (guint64) rlim_nofile;
				if (0 != setrlimit(RLIMIT_NOFILE, &rlim)) {
					ERROR(srv, "couldn't set 'max filedescriptors': %s", g_strerror(errno));
				}
			}
		}
#endif

		if (gid != (gid_t) -1) {
			if (-1 == setgid(gid)) {
				ERROR(srv, "setgid(%i) failed: %s", (int) gid, g_strerror(errno));
				abort();
			}
			if (-1 == setgroups(0, NULL)) {
				ERROR(srv, "setgroups failed: %s", g_strerror(errno));
				abort();
			}
			if (username && -1 == initgroups(username, gid)) {
				ERROR(srv, "initgroups('%s', %i) failed: %s", username, (int) gid, g_strerror(errno));
				abort();
			}
		}

		if (cb) cb(ctx);

		if (uid != (uid_t) -1) {
			if (-1 == setuid(uid)) {
				ERROR(srv, "setuid(%i) failed: %s", (int) uid, g_strerror(errno));
				abort();
			}
		}

		if (NULL == env)
			execv(args[0], args);
		else
			execve(args[0], args, env);

		g_printerr("exec('%s') failed: %s\n", args[0], g_strerror(errno));
		abort();

		break;
	case -1:
		ERROR(srv, "fork failed: %s", g_strerror(errno));
		li_proc_free(proc);
		return NULL;
	default:
		proc->child_pid = pid;
		li_error_pipe_activate(proc->epipe);
		break;
	}

	return proc;
}

void li_proc_free(liProc *proc) {
	li_error_pipe_free(proc->epipe);
	g_free(proc->appname);
	g_slice_free(liProc, proc);
}

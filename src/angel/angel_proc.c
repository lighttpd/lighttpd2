
#include <lighttpd/angel_base.h>

#include <grp.h>

static void read_pipe(liServer *srv, liErrorPipe *epipe, gboolean flush) {
	const ssize_t max_read = 1024;
	ssize_t r, toread;
	GString *buf;
	int count = 10;

	if (-1 == epipe->fds[0]) return;

	for (;;) {
		if (ioctl(epipe->fds[0], FIONREAD, &toread) || toread == 0) {
			toread = 256;
		} else {
			if (toread > max_read) toread = max_read;
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
	ev_io_stop(srv->loop, &epipe->fd_watcher);
	close(epipe->fds[0]);
	epipe->fds[0] = -1;
}

static void error_pipe_cb(struct ev_loop *loop, ev_io *w, int revents) {
	liErrorPipe *epipe = w->data;
	UNUSED(loop);
	UNUSED(revents);

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
	ev_io_init(&epipe->fd_watcher, error_pipe_cb, fds[0], EV_READ);
	epipe->fd_watcher.data = epipe;
	epipe->fds[0] = fds[0];
	epipe->fds[1] = fds[1];

	li_fd_init(fds[0]);

	return epipe;
}

void li_error_pipe_free(liErrorPipe *epipe) {
	liServer *srv = epipe->srv;

	ev_io_stop(srv->loop, &epipe->fd_watcher);
	li_error_pipe_flush(epipe);
	if (-1 != epipe->fds[0]) close(epipe->fds[0]);
	if (-1 != epipe->fds[1]) close(epipe->fds[1]);

	g_slice_free(liErrorPipe, epipe);
}

/** closes out-fd */
void li_error_pipe_activate(liErrorPipe *epipe) {
	liServer *srv = epipe->srv;

	if (-1 != epipe->fds[1]) close(epipe->fds[1]);
	ev_io_start(srv->loop, &epipe->fd_watcher);
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

	ERROR(srv, "%s (pid: %i): %s", proc->appname, proc->child_pid, msg->str);
}

liProc* li_proc_new(liServer *srv, gchar **args, gchar **env, uid_t uid, gid_t gid, gchar *username, liProcSetupCB cb, gpointer ctx) {
	liProc *proc;
	pid_t pid;

	proc = g_slice_new0(liProc);
	proc->srv = srv;
	proc->child_pid = -1;
	proc->epipe = li_error_pipe_new(srv, proc_epipe_cb, proc);
	proc->appname = g_strdup(args[0]);

	switch (pid = fork()) {
	case 0:
		li_error_pipe_use(proc->epipe, STDERR_FILENO);

		setsid();

		if (gid != (gid_t) -1) {
			setgid(gid);
			setgroups(0, NULL);
			if (username) initgroups(username, gid);
		}

		if (cb) cb(ctx);

		if (uid != (uid_t) -1) {
			setuid(uid);
		}

		if (NULL == env) env = environ;

		execve(args[0], args, env);
		g_printerr("exec('%s') failed: %s\n", args[0], g_strerror(errno));
		exit(-1);

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

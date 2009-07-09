
#include <lighttpd/angel_base.h>

#include <grp.h>

static void instance_state_machine(liInstance *i);

static void jobqueue_callback(struct ev_loop *loop, ev_async *w, int revents) {
	liServer *srv = (liServer*) w->data;
	liInstance *i;
	GQueue todo;
	UNUSED(loop);
	UNUSED(revents);

	todo = srv->job_queue;
	g_queue_init(&srv->job_queue);

	while (NULL != (i = g_queue_pop_head(&todo))) {
		i->in_jobqueue = FALSE;
		instance_state_machine(i);
		li_instance_release(i);
	}
}

liServer* li_server_new(const gchar *module_dir) {
	liServer *srv = g_slice_new0(liServer);

	srv->loop = ev_default_loop(0);

	/* TODO: handle signals */
	ev_async_init(&srv->job_watcher, jobqueue_callback);
	ev_async_start(srv->loop, &srv->job_watcher);
	ev_unref(srv->loop);
	srv->job_watcher.data = srv;

	log_init(srv);
	plugins_init(srv, module_dir);
	return srv;
}

void li_server_free(liServer* srv) {
	plugins_clear(srv);

	log_clean(srv);
	g_slice_free(liServer, srv);
}

static void instance_angel_call_cb(liAngelConnection *acon,
		const gchar *mod, gsize mod_len, const gchar *action, gsize action_len,
		gint32 id,
		GString *data) {

	liInstance *i = (liInstance*) acon->data;
	liServer *srv = i->srv;
	liPlugins *ps = &srv->plugins;
	liPlugin *p;
	liPluginHandleCallCB cb;

	p = g_hash_table_lookup(ps->ht_plugins, mod);
	if (!p) {
		GString *errstr = g_string_sized_new(0);
		GError *err = NULL;
		g_string_printf(errstr, "Plugin '%s' not available in lighttpd-angel", mod);
		if (!li_angel_send_result(acon, id, errstr, NULL, NULL, &err)) {
			ERROR(srv, "Couldn't send result: %s", err->message);
			g_error_free(err);
		}
		return;
	}

	cb = (liPluginHandleCallCB)(intptr_t) g_hash_table_lookup(p->angel_callbacks, action);
	if (!cb) {
		GString *errstr = g_string_sized_new(0);
		GError *err = NULL;
		g_string_printf(errstr, "Action '%s' not available in plugin '%s' of lighttpd-angel", action, mod);
		if (!li_angel_send_result(acon, id, errstr, NULL, NULL, &err)) {
			ERROR(srv, "Couldn't send result: %s", err->message);
			g_error_free(err);
		}
		return;
	}

	cb(srv, i, p, id, data);
}

static void instance_angel_close_cb(liAngelConnection *acon, GError *err) {
	liInstance *i = (liInstance*) acon->data;
	liServer *srv = i->srv;

	ERROR(srv, "angel connection closed: %s", err ? err->message : g_strerror(errno));
	if (err) g_error_free(err);

	i->acon = NULL;
	li_angel_connection_free(acon);
}

static void instance_child_cb(struct ev_loop *loop, ev_child *w, int revents) {
	liInstance *i = (liInstance*) w->data;

	if (i->s_cur == LI_INSTANCE_LOADING) {
		ERROR(i->srv, "spawning child %i failed, not restarting", i->pid);
		i->s_dest = i->s_cur = LI_INSTANCE_DOWN; /* TODO: retry spawn later? */
	} else {
		ERROR(i->srv, "child %i died", i->pid);
		i->s_cur = LI_INSTANCE_DOWN;
	}
	i->pid = -1;
	li_angel_connection_free(i->acon);
	i->acon = NULL;
	ev_child_stop(loop, w);
	li_instance_job_append(i);
	li_instance_release(i);
}

static void instance_spawn(liInstance *i) {
	int confd[2];
	if (-1 == socketpair(AF_UNIX, SOCK_STREAM, 0, confd)) {
		ERROR(i->srv, "socketpair error, cannot spawn instance: %s", g_strerror(errno));
		return;
	}
	li_fd_init(confd[0]);
	li_fd_no_block(confd[1]);

	i->acon = li_angel_connection_new(i->srv->loop, confd[0], i, instance_angel_call_cb, instance_angel_close_cb);
	i->pid = fork();
	switch (i->pid) {
	case 0: {
		gchar **args;
		setsid(); /* lead session, so we don't recieve the signals for the angel */
		if (getuid() == 0 && (i->ic->uid != (uid_t) -1) && (i->ic->gid != (gid_t) -1)) {
			setgid(i->ic->gid);
			setgroups(0, NULL);
			initgroups(i->ic->username->str, i->ic->gid);
			setuid(i->ic->uid);
		}

		if (confd[1] != 0) {
			dup2(confd[1], 0);
			close(confd[1]);
		}
		/* TODO: close stdout/stderr ? */
		execvp(i->ic->cmd[0], i->ic->cmd);
		g_printerr("exec('%s') failed: %s\n", i->ic->cmd[0], g_strerror(errno));
		exit(-1);
	}
	case -1:
		break;
	default:
		close(confd[1]);
		ev_child_set(&i->child_watcher, i->pid, 0);
		ev_child_start(i->srv->loop, &i->child_watcher);
		i->s_cur = LI_INSTANCE_LOADING;
		li_instance_acquire(i);
		ERROR(i->srv, "Instance (%i) spawned: %s", i->pid, i->ic->cmd[0]);
		break;
	}
}

liInstance* li_server_new_instance(liServer *srv, liInstanceConf *ic) {
	liInstance *i;

	i = g_slice_new0(liInstance);
	i->refcount = 1;
	i->srv = srv;
	li_instance_conf_acquire(ic);
	i->ic = ic;
	i->pid = -1;
	i->s_cur = i->s_dest = LI_INSTANCE_DOWN;
	ev_child_init(&i->child_watcher, instance_child_cb, -1, 0);
	i->child_watcher.data = i;

	return i;
}

void li_instance_replace(liInstance *oldi, liInstance *newi) {
}

void li_instance_set_state(liInstance *i, liInstanceState s) {
	if (i->s_dest == s) return;
	switch (s) {
	case LI_INSTANCE_DOWN:
		break;
	case LI_INSTANCE_LOADING:
	case LI_INSTANCE_WARMUP:
		return; /* These cannot be set */
	case LI_INSTANCE_ACTIVE:
	case LI_INSTANCE_SUSPEND:
		break;
	}
	i->s_dest = s;
	if (s == LI_INSTANCE_DOWN) {
		if (i->s_cur != LI_INSTANCE_DOWN) {
			kill(i->pid, SIGTERM);
		}
	} else {
		if (i->pid == (pid_t) -1) {
			instance_spawn(i);
			return;
		} else {
			GError *error = NULL;
			GString *buf = g_string_sized_new(0);

			switch (s) {
			case LI_INSTANCE_DOWN:
			case LI_INSTANCE_LOADING:
			case LI_INSTANCE_WARMUP:
				break;
			case LI_INSTANCE_ACTIVE:
				li_angel_send_simple_call(i->acon, CONST_STR_LEN("core"), CONST_STR_LEN("run"), buf, &error);
				break;
			case LI_INSTANCE_SUSPEND:
				li_angel_send_simple_call(i->acon, CONST_STR_LEN("core"), CONST_STR_LEN("suspend"), buf, &error);
				break;
			}
		}
	}
}

static void instance_state_machine(liInstance *i) {
	liInstanceState olds = i->s_dest;
	while (i->s_cur != i->s_dest && i->s_cur != olds) {
		olds = i->s_cur;
		switch (i->s_dest) {
		case LI_INSTANCE_DOWN:
			if (i->pid == (pid_t) -1) {
				i->s_cur = LI_INSTANCE_DOWN;
				break;
			}
			kill(i->pid, SIGINT);
			return;
		case LI_INSTANCE_LOADING:
			break;
		case LI_INSTANCE_WARMUP:
			if (i->pid == (pid_t) -1) {
				instance_spawn(i);
				return;
			}
			break;
		case LI_INSTANCE_ACTIVE:
			if (i->pid == (pid_t) -1) {
				instance_spawn(i);
				return;
			}
			break;
		case LI_INSTANCE_SUSPEND:
			if (i->pid == (pid_t) -1) {
				instance_spawn(i);
				return;
			}
			break;
		}
	}
}

void li_instance_release(liInstance *i) {
	liServer *srv;
	liInstance *t;
	if (!i) return;
	assert(g_atomic_int_get(&i->refcount) > 0);
	if (!g_atomic_int_dec_and_test(&i->refcount)) return;
	srv = i->srv;
	if (i->pid != (pid_t) -1) {
		ev_child_stop(srv->loop, &i->child_watcher);
		kill(i->pid, SIGTERM);
		i->pid = -1;
		i->s_cur = LI_INSTANCE_DOWN;
		li_angel_connection_free(i->acon);
		i->acon = NULL;
	}

	li_instance_conf_release(i->ic);
	i->ic = NULL;

	t = i->replace; i->replace = NULL;
	li_instance_release(t);

	t = i->replace_by; i->replace_by = NULL;
	li_instance_release(t);

	g_slice_free(liInstance, i);
}

void li_instance_acquire(liInstance *i) {
	assert(g_atomic_int_get(&i->refcount) > 0);
	g_atomic_int_inc(&i->refcount);
}

liInstanceConf* li_instance_conf_new(gchar **cmd, GString *username, uid_t uid, gid_t gid) {
	liInstanceConf *ic = g_slice_new0(liInstanceConf);
	ic->refcount = 1;
	ic->cmd = cmd;
	if (username) {
		ic->username = g_string_new_len(GSTR_LEN(username));
	}
	ic->uid = uid;
	ic->gid = gid;
	return ic;
}

void li_instance_conf_release(liInstanceConf *ic) {
	if (!ic) return;
	assert(g_atomic_int_get(&ic->refcount) > 0);
	if (!g_atomic_int_dec_and_test(&ic->refcount)) return;
	g_strfreev(ic->cmd);
	g_slice_free(liInstanceConf, ic);
}

void li_instance_conf_acquire(liInstanceConf *ic) {
	assert(g_atomic_int_get(&ic->refcount) > 0);
	g_atomic_int_inc(&ic->refcount);
}

void li_instance_job_append(liInstance *i) {
	liServer *srv = i->srv;
	if (!i->in_jobqueue) {
		li_instance_acquire(i);
		i->in_jobqueue = TRUE;
		g_queue_push_tail(&srv->job_queue, i);
		ev_async_send(srv->loop, &srv->job_watcher);
	}
}

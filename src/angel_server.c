
#include <lighttpd/angel_base.h>

#include <grp.h>

static void instance_state_machine(instance *i);

static void jobqueue_callback(struct ev_loop *loop, ev_async *w, int revents) {
	server *srv = (server*) w->data;
	instance *i;
	GQueue todo;
	UNUSED(loop);
	UNUSED(revents);

	todo = srv->job_queue;
	g_queue_init(&srv->job_queue);

	while (NULL != (i = g_queue_pop_head(&todo))) {
		i->in_jobqueue = FALSE;
		instance_state_machine(i);
		instance_release(i);
	}
}

server* server_new(const gchar *module_dir) {
	server *srv = g_slice_new0(server);

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

void server_free(server* srv) {
	plugins_clear(srv);

	log_clean(srv);
	g_slice_free(server, srv);
}

static void instance_angel_call_cb(angel_connection *acon,
		const gchar *mod, gsize mod_len, const gchar *action, gsize action_len,
		gint32 id,
		GString *data) {

	instance *i = (instance*) acon->data;
	server *srv = i->srv;
	Plugins *ps = &srv->plugins;
	plugin *p;
	PluginHandleCall cb;

	p = g_hash_table_lookup(ps->ht_plugins, mod);
	if (!p) {
		GString *errstr = g_string_sized_new(0);
		GError *err = NULL;
		g_string_printf(errstr, "Plugin '%s' not available in lighttpd-angel", mod);
		if (!angel_send_result(acon, id, errstr, NULL, NULL, &err)) {
			ERROR(srv, "Couldn't send result: %s", err->message);
			g_error_free(err);
		}
		return;
	}

	cb = (PluginHandleCall)(intptr_t) g_hash_table_lookup(p->angel_callbacks, action);
	if (!cb) {
		GString *errstr = g_string_sized_new(0);
		GError *err = NULL;
		g_string_printf(errstr, "Action '%s' not available in plugin '%s' of lighttpd-angel", action, mod);
		if (!angel_send_result(acon, id, errstr, NULL, NULL, &err)) {
			ERROR(srv, "Couldn't send result: %s", err->message);
			g_error_free(err);
		}
		return;
	}

	cb(srv, i, p, id, data);
}

static void instance_angel_close_cb(angel_connection *acon, GError *err) {
	instance *i = (instance*) acon->data;
	server *srv = i->srv;

	ERROR(srv, "angel connection closed: %s", err ? err->message : g_strerror(errno));
	if (err) g_error_free(err);

	i->acon = NULL;
	angel_connection_free(acon);
}

static void instance_child_cb(struct ev_loop *loop, ev_child *w, int revents) {
	instance *i = (instance*) w->data;

	if (i->s_cur == INSTANCE_LOADING) {
		ERROR(i->srv, "spawning child %i failed, not restarting", i->pid);
		i->s_dest = i->s_cur = INSTANCE_DOWN; /* TODO: retry spawn later? */
	} else {
		ERROR(i->srv, "child %i died", i->pid);
		i->s_cur = INSTANCE_DOWN;
	}
	i->pid = -1;
	angel_connection_free(i->acon);
	i->acon = NULL;
	ev_child_stop(loop, w);
	instance_job_append(i);
	instance_release(i);
}

static void instance_spawn(instance *i) {
	int confd[2];
	if (-1 == socketpair(AF_UNIX, SOCK_STREAM, 0, confd)) {
		ERROR(i->srv, "socketpair error, cannot spawn instance: %s", g_strerror(errno));
		return;
	}
	fd_init(confd[0]);
	fd_no_block(confd[1]);

	i->acon = angel_connection_new(i->srv->loop, confd[0], i, instance_angel_call_cb, instance_angel_close_cb);
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
		i->s_cur = INSTANCE_LOADING;
		instance_acquire(i);
		ERROR(i->srv, "Instance (%i) spawned: %s", i->pid, i->ic->cmd[0]);
		break;
	}
}

instance* server_new_instance(server *srv, instance_conf *ic) {
	instance *i;

	i = g_slice_new0(instance);
	i->refcount = 1;
	i->srv = srv;
	instance_conf_acquire(ic);
	i->ic = ic;
	i->pid = -1;
	i->s_cur = i->s_dest = INSTANCE_DOWN;
	ev_child_init(&i->child_watcher, instance_child_cb, -1, 0);
	i->child_watcher.data = i;

	return i;
}

void instance_replace(instance *oldi, instance *newi) {
}

void instance_set_state(instance *i, instance_state_t s) {
	if (i->s_dest == s) return;
	switch (s) {
	case INSTANCE_DOWN:
		break;
	case INSTANCE_LOADING:
	case INSTANCE_WARMUP:
		return; /* These cannot be set */
	case INSTANCE_ACTIVE:
	case INSTANCE_SUSPEND:
		break;
	}
	i->s_dest = s;
	if (s == INSTANCE_DOWN) {
		if (i->s_cur != INSTANCE_DOWN) {
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
			case INSTANCE_DOWN:
			case INSTANCE_LOADING:
			case INSTANCE_WARMUP:
				break;
			case INSTANCE_ACTIVE:
				angel_send_simple_call(i->acon, CONST_STR_LEN("core"), CONST_STR_LEN("run"), buf, &error);
				break;
			case INSTANCE_SUSPEND:
				angel_send_simple_call(i->acon, CONST_STR_LEN("core"), CONST_STR_LEN("suspend"), buf, &error);
				break;
			}
		}
	}
}

static void instance_state_machine(instance *i) {
	instance_state_t olds = i->s_dest;
	while (i->s_cur != i->s_dest && i->s_cur != olds) {
		olds = i->s_cur;
		switch (i->s_dest) {
		case INSTANCE_DOWN:
			if (i->pid == (pid_t) -1) {
				i->s_cur = INSTANCE_DOWN;
				break;
			}
			kill(i->pid, SIGINT);
			return;
		case INSTANCE_LOADING:
			break;
		case INSTANCE_WARMUP:
			if (i->pid == (pid_t) -1) {
				instance_spawn(i);
				return;
			}
			break;
		case INSTANCE_ACTIVE:
			if (i->pid == (pid_t) -1) {
				instance_spawn(i);
				return;
			}
			break;
		case INSTANCE_SUSPEND:
			if (i->pid == (pid_t) -1) {
				instance_spawn(i);
				return;
			}
			break;
		}
	}
}

void instance_release(instance *i) {
	server *srv;
	instance *t;
	if (!i) return;
	assert(g_atomic_int_get(&i->refcount) > 0);
	if (!g_atomic_int_dec_and_test(&i->refcount)) return;
	srv = i->srv;
	if (i->pid != (pid_t) -1) {
		ev_child_stop(srv->loop, &i->child_watcher);
		kill(i->pid, SIGTERM);
		i->pid = -1;
		i->s_cur = INSTANCE_DOWN;
		angel_connection_free(i->acon);
		i->acon = NULL;
	}

	instance_conf_release(i->ic);
	i->ic = NULL;

	t = i->replace; i->replace = NULL;
	instance_release(t);

	t = i->replace_by; i->replace_by = NULL;
	instance_release(t);

	g_slice_free(instance, i);
}

void instance_acquire(instance *i) {
	assert(g_atomic_int_get(&i->refcount) > 0);
	g_atomic_int_inc(&i->refcount);
}

instance_conf* instance_conf_new(gchar **cmd, GString *username, uid_t uid, gid_t gid) {
	instance_conf *ic = g_slice_new0(instance_conf);
	ic->refcount = 1;
	ic->cmd = cmd;
	if (username) {
		ic->username = g_string_new_len(GSTR_LEN(username));
	}
	ic->uid = uid;
	ic->gid = gid;
	return ic;
}

void instance_conf_release(instance_conf *ic) {
	if (!ic) return;
	assert(g_atomic_int_get(&ic->refcount) > 0);
	if (!g_atomic_int_dec_and_test(&ic->refcount)) return;
	g_strfreev(ic->cmd);
	g_slice_free(instance_conf, ic);
}

void instance_conf_acquire(instance_conf *ic) {
	assert(g_atomic_int_get(&ic->refcount) > 0);
	g_atomic_int_inc(&ic->refcount);
}

void instance_job_append(instance *i) {
	server *srv = i->srv;
	if (!i->in_jobqueue) {
		instance_acquire(i);
		i->in_jobqueue = TRUE;
		g_queue_push_tail(&srv->job_queue, i);
		ev_async_send(srv->loop, &srv->job_watcher);
	}
}

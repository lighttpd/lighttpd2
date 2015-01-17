
#include <lighttpd/angel_base.h>

static void sigint_cb(liEventBase *watcher, int events) {
	liServer *srv = LI_CONTAINER_OF(li_event_get_loop_(watcher), liServer, loop);
	UNUSED(events);

	li_server_stop(srv);
}

static void sigpipe_cb(liEventBase *watcher, int events) {
	/* ignore */
	UNUSED(watcher); UNUSED(events);
}

liServer* li_server_new(const gchar *module_dir, gboolean module_resident) {
	liServer *srv = g_slice_new0(liServer);

	li_event_loop_init(&srv->loop, ev_default_loop(0));

	li_event_signal_init(&srv->loop, "angel SIGINT", &srv->sig_w_INT, sigint_cb, SIGINT);
	li_event_signal_init(&srv->loop, "angel SIGTERM", &srv->sig_w_TERM, sigint_cb, SIGTERM);
	li_event_signal_init(&srv->loop, "angel SIGPIPE", &srv->sig_w_PIPE, sigpipe_cb, SIGPIPE);

	li_log_init(srv);
	li_plugins_init(srv, module_dir, module_resident);
	return srv;
}

void li_server_free(liServer* srv) {
	li_plugins_clear(srv);

	li_log_clean(srv);

	li_event_clear(&srv->sig_w_INT);
	li_event_clear(&srv->sig_w_TERM);
	li_event_clear(&srv->sig_w_PIPE);

	li_event_loop_clear(&srv->loop);
	ev_default_destroy();

	g_slice_free(liServer, srv);
}

void li_server_stop(liServer *srv) {
	li_event_stop(&srv->sig_w_INT);
	li_event_stop(&srv->sig_w_TERM);

	li_plugins_config_load(srv, NULL);

	li_event_loop_end(&srv->loop);
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
	UNUSED(mod_len);
	UNUSED(action_len);

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

	cb(srv, p, i, id, data);
}

static void instance_angel_close_cb(liAngelConnection *acon, GError *err) {
	liInstance *i = (liInstance*) acon->data;
	liServer *srv = i->srv;
	gboolean hide_error = FALSE;

	if (err) {
		if (LI_INSTANCE_FINISHED == i->s_dest &&
			LI_ANGEL_CONNECTION_RESET == err->code &&
			LI_ANGEL_CONNECTION_ERROR == err->domain) hide_error = TRUE;
	}
	if (!hide_error)
		ERROR(srv, "angel connection closed: %s", err ? err->message : g_strerror(errno));
	if (err) g_error_free(err);

	i->acon = NULL;
	li_angel_connection_free(acon);
}

static void instance_child_cb(liEventBase *watcher, int events) {
	liInstance *i = LI_CONTAINER_OF(li_event_child_from(watcher), liInstance, child_watcher);
	liInstanceState news;
	int status = li_event_child_status(&i->child_watcher);
	UNUSED(events);

	if (i->s_dest == LI_INSTANCE_FINISHED) {
		if (WIFEXITED(status)) {
			if (0 != WEXITSTATUS(status)) {
				ERROR(i->srv, "child %i died with exit status %i", i->proc->child_pid, WEXITSTATUS(status));
			} /* exit status 0 is ok, no message */
		} else if (WIFSIGNALED(status)) {
			ERROR(i->srv, "child %i died: killed by '%s' (%i)", i->proc->child_pid, g_strsignal(WTERMSIG(status)), WTERMSIG(status));
		} else {
			ERROR(i->srv, "child %i died with unexpected stat_val %i", i->proc->child_pid, status);
		}
		news = LI_INSTANCE_FINISHED;
	} else {
		if (WIFEXITED(status)) {
			ERROR(i->srv, "child %i died with exit status %i", i->proc->child_pid, WEXITSTATUS(status));
		} else if (WIFSIGNALED(status)) {
			ERROR(i->srv, "child %i died: killed by %s", i->proc->child_pid, g_strsignal(WTERMSIG(status)));
		} else {
			ERROR(i->srv, "child %i died with unexpected stat_val %i", i->proc->child_pid, status);
		}
		if (i->s_cur == LI_INSTANCE_DOWN) {
			ERROR(i->srv, "spawning child %i failed, not restarting", i->proc->child_pid);
			news = i->s_dest = LI_INSTANCE_FINISHED; /* TODO: retry spawn later? */
		} else {
			news = LI_INSTANCE_DOWN;
		}
	}
	li_proc_free(i->proc);
	i->proc = NULL;
	li_angel_connection_free(i->acon);
	i->acon = NULL;
	li_event_clear(&i->child_watcher);
	li_instance_state_reached(i, news);
	li_instance_release(i);
}

static void instance_spawn_setup(gpointer ctx) {
	int *confd = ctx;

	if (confd[1] != 0) {
		dup2(confd[1], 0);
		close(confd[1]);
	}

	dup2(STDERR_FILENO, STDOUT_FILENO);
}

static void instance_spawn(liInstance *i) {
	int confd[2];
	if (-1 == socketpair(AF_UNIX, SOCK_STREAM, 0, confd)) {
		ERROR(i->srv, "socketpair error, cannot spawn instance: %s", g_strerror(errno));
		return;
	}
	li_fd_init(confd[0]);
	li_fd_no_block(confd[1]);

	i->acon = li_angel_connection_new(&i->srv->loop, confd[0], i, instance_angel_call_cb, instance_angel_close_cb);
	i->proc = li_proc_new(i->srv, i->ic->cmd, i->ic->env, i->ic->uid, i->ic->gid,
		i->ic->username != NULL ? i->ic->username->str : NULL, i->ic->rlim_core, i->ic->rlim_nofile, instance_spawn_setup, confd);

	if (!i->proc) return;

	close(confd[1]);
	li_event_clear(&i->child_watcher);
	li_event_child_init(&i->srv->loop, "lighttpd2-worker", &i->child_watcher, instance_child_cb, i->proc->child_pid);
	i->s_cur = LI_INSTANCE_DOWN;
	li_instance_acquire(i);
	DEBUG(i->srv, "Instance (%i) spawned: %s", i->proc->child_pid, i->ic->cmd[0]);
}

liInstance* li_server_new_instance(liServer *srv, liInstanceConf *ic) {
	liInstance *i;

	i = g_slice_new0(liInstance);
	i->refcount = 1;
	i->srv = srv;
	li_instance_conf_acquire(ic);
	i->ic = ic;
	i->s_cur = i->s_dest = LI_INSTANCE_DOWN;
	i->resources = g_ptr_array_new();

	return i;
}

gboolean li_instance_replace(liInstance *oldi, liInstance *newi) {
	if (oldi->replace_by || newi->replace) return FALSE;
	oldi->replace_by = newi;
	newi->replace = oldi;
	li_instance_acquire(oldi);
	li_instance_acquire(newi);

	li_instance_set_state(newi, LI_INSTANCE_WARMUP);

	return TRUE;
}

static void li_instance_unset_replace(liInstance *oldi, liInstance *newi) {
	g_assert(newi == oldi->replace_by); oldi->replace_by = NULL;
	g_assert(oldi == newi->replace); newi->replace = NULL;

	li_angel_plugin_replaced_instance(oldi->srv, oldi, newi);

	li_instance_release(oldi);
	li_instance_release(newi);
}

void li_instance_set_state(liInstance *i, liInstanceState s) {
	if (i->s_dest == s) return;
	switch (s) {
	case LI_INSTANCE_DOWN:
	case LI_INSTANCE_SUSPENDING:
		ERROR(i->srv, "Invalid destination state %i", s);
		return; /* cannot set this */
	case LI_INSTANCE_WARMUP:
	case LI_INSTANCE_SUSPENDED:
	case LI_INSTANCE_RUNNING:
	case LI_INSTANCE_FINISHED:
		break;
	}
	i->s_dest = s;
	if (!i->proc && LI_INSTANCE_FINISHED != s) {
		instance_spawn(i);
	} else {
		GError *error = NULL;

		switch (s) {
		case LI_INSTANCE_DOWN:
		case LI_INSTANCE_SUSPENDING:
			break; /* cannot be set */
		case LI_INSTANCE_WARMUP:
			li_angel_send_simple_call(i->acon, CONST_STR_LEN("core"), CONST_STR_LEN("warmup"), NULL, &error);
			break;
		case LI_INSTANCE_SUSPENDED:
			li_angel_send_simple_call(i->acon, CONST_STR_LEN("core"), CONST_STR_LEN("suspend"), NULL, &error);
			break;
		case LI_INSTANCE_RUNNING:
			li_angel_send_simple_call(i->acon, CONST_STR_LEN("core"), CONST_STR_LEN("run"), NULL, &error);
			break;
		case LI_INSTANCE_FINISHED:
			if (i->proc) {
				kill(i->proc->child_pid, SIGTERM);
			} else {
				li_instance_state_reached(i, LI_INSTANCE_FINISHED);
			}
			break;
		}

		if (error) {
			GERROR(i->srv, error, "set state %i failed, killing instance:", s);
			g_error_free(error);
			if (i->proc) {
				kill(i->proc->child_pid, SIGTERM);
			} else {
				li_instance_state_reached(i, LI_INSTANCE_FINISHED);
			}
		}
	}
}

void li_instance_state_reached(liInstance *i, liInstanceState s) {
	GError *error = NULL;

	i->s_cur = s;
	switch (s) {
	case LI_INSTANCE_DOWN:
		/* last child died */
		if (i->s_dest == LI_INSTANCE_FINISHED) {
			i->s_cur = LI_INSTANCE_FINISHED;
		} else if (!i->srv->one_shot) {
			instance_spawn(i);
		}
		break;
	case LI_INSTANCE_SUSPENDED:
		if (i->replace_by && i->replace_by->s_dest == LI_INSTANCE_WARMUP) {
			li_instance_set_state(i->replace_by, LI_INSTANCE_RUNNING);
		}
		switch (i->s_dest) {
		case LI_INSTANCE_DOWN:
			break; /* impossible */
		case LI_INSTANCE_SUSPENDED:
			break;
		case LI_INSTANCE_WARMUP:
			/* make sure we move to SILENT after we spawned the instance */
			li_angel_send_simple_call(i->acon, CONST_STR_LEN("core"), CONST_STR_LEN("warmup"), NULL, &error);
			break;
		case LI_INSTANCE_RUNNING:
			/* make sure we move to RUNNING after we spawned the instance */
			li_angel_send_simple_call(i->acon, CONST_STR_LEN("core"), CONST_STR_LEN("run"), NULL, &error);
			break;
		case LI_INSTANCE_SUSPENDING:
		case LI_INSTANCE_FINISHED:
			break; /* nothing to do, instance should already know what to do */
		}
		break;
	case LI_INSTANCE_WARMUP:
		if (i->replace) {
			/* stop old instance */
			li_instance_set_state(i->replace, LI_INSTANCE_FINISHED);
		}
		break;
	case LI_INSTANCE_RUNNING:
		/* nothing to do, instance should already know what to do */
		break;
	case LI_INSTANCE_SUSPENDING:
		/* nothing to do, instance should already know what to do */
		break;
	case LI_INSTANCE_FINISHED:
		if (i->replace) {
			ERROR(i->srv, "%s", "Replacing instance failed, continue old instance");
			li_instance_set_state(i->replace, LI_INSTANCE_RUNNING);

			li_instance_unset_replace(i->replace, i);
		} else if (i->s_dest == LI_INSTANCE_FINISHED) {
			if (i->replace_by) {
				INFO(i->srv, "%s", "Instance replaced");
				if (i->replace_by->s_dest == LI_INSTANCE_WARMUP) {
					li_instance_set_state(i->replace_by, LI_INSTANCE_RUNNING);
				}
				li_instance_unset_replace(i, i->replace_by);
			}
		}
		break;
	}

	if (error) {
		GERROR(i->srv, error, "reaching state %i failed, killing instance:", s);
		g_error_free(error);
		if (i->proc) {
			kill(i->proc->child_pid, SIGTERM);
		} else {
			li_instance_state_reached(i, LI_INSTANCE_FINISHED);
		}
	} else {
		li_angel_plugin_instance_reached_state(i->srv, i, s);
	}
}

void li_instance_release(liInstance *i) {
	liServer *srv;
	liInstance *t;
	guint j;

	if (!i) return;
	srv = i->srv;

	g_assert(g_atomic_int_get(&i->refcount) > 0);

	if (!g_atomic_int_dec_and_test(&i->refcount)) return;
	g_assert(!i->proc);

	DEBUG(srv, "%s", "instance released");

	li_instance_conf_release(i->ic);
	i->ic = NULL;

	t = i->replace; i->replace = NULL;
	li_instance_release(t);

	t = i->replace_by; i->replace_by = NULL;
	li_instance_release(t);

	for (j = 0; j < i->resources->len; j++) {
		liInstanceResource *res = g_ptr_array_index(i->resources, j);
		res->ndx = -1;
		res->free_cb(srv, i, res->plugin, res);
	}

	g_ptr_array_free(i->resources, TRUE);

	g_slice_free(liInstance, i);
}

void li_instance_acquire(liInstance *i) {
	LI_FORCE_ASSERT(g_atomic_int_get(&i->refcount) > 0);
	g_atomic_int_inc(&i->refcount);
}

liInstanceConf* li_instance_conf_new(gchar **cmd, gchar **env, GString *username, uid_t uid, gid_t gid, gint64 rlim_core, gint64 rlim_nofile) {
	liInstanceConf *ic = g_slice_new0(liInstanceConf);
	ic->refcount = 1;
	ic->cmd = cmd;
	ic->env = env;
	ic->username = username;
	ic->uid = uid;
	ic->gid = gid;
	ic->rlim_core = rlim_core;
	ic->rlim_nofile = rlim_nofile;
	return ic;
}

void li_instance_conf_release(liInstanceConf *ic) {
	if (!ic) return;
	LI_FORCE_ASSERT(g_atomic_int_get(&ic->refcount) > 0);
	if (!g_atomic_int_dec_and_test(&ic->refcount)) return;

	if (ic->username) g_string_free(ic->username, TRUE);
	g_strfreev(ic->cmd);
	g_strfreev(ic->env);
	g_slice_free(liInstanceConf, ic);
}

void li_instance_conf_acquire(liInstanceConf *ic) {
	LI_FORCE_ASSERT(g_atomic_int_get(&ic->refcount) > 0);
	g_atomic_int_inc(&ic->refcount);
}

void li_instance_add_resource(liInstance *i, liInstanceResource *res, liInstanceResourceFreeCB free_cb, liPlugin *p, gpointer data) {
	res->free_cb = free_cb;
	res->data = data;
	res->plugin = p;
	res->ndx = i->resources->len;

	g_ptr_array_add(i->resources, res);
}

void li_instance_rem_resource(liInstance *i, liInstanceResource *res) {
	liInstanceResource *res2;
	g_assert(res == g_ptr_array_index(i->resources, res->ndx));

	g_ptr_array_remove_index_fast(i->resources, res->ndx);
	res2 = g_ptr_array_index(i->resources, res->ndx);
	res2->ndx = res->ndx;
}

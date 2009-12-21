
#include <lighttpd/base.h>

static gboolean plugin_load_default_option(liServer *srv, liServerOption *sopt);
static void li_plugin_free_default_options(liServer *srv, liPlugin *p);

static liPlugin* plugin_new(const gchar *name) {
	liPlugin *p = g_slice_new0(liPlugin);
	p->name = name;
	return p;
}

static void li_plugin_free_options(liServer *srv, liPlugin *p) {
	size_t i;
	const liPluginOption *po;
	liServerOption *so;

	if (!p->options) return;
	for (i = 0; (po = &p->options[i])->name; i++) {
		if (NULL == (so = g_hash_table_lookup(srv->options, po->name))) break;
		if (so->p != p) break;
		g_hash_table_remove(srv->options, po->name);
	}
}

static void li_plugin_free_actions(liServer *srv, liPlugin *p) {
	size_t i;
	const liPluginAction *pa;
	liServerAction *sa;

	if (!p->actions) return;
	for (i = 0; (pa = &p->actions[i])->name; i++) {
		if (NULL == (sa = g_hash_table_lookup(srv->actions, pa->name))) break;
		if (sa->p != p) break;
		g_hash_table_remove(srv->actions, pa->name);
	}
}

static void li_plugin_free_setups(liServer *srv, liPlugin *p) {
	size_t i;
	const liPluginSetup *ps;
	liServerSetup *ss;

	if (!p->setups) return;
	for (i = 0; (ps = &p->setups[i])->name; i++) {
		if (NULL == (ss = g_hash_table_lookup(srv->setups, ps->name))) break;
		if (ss->p != p) break;
		g_hash_table_remove(srv->setups, ps->name);
	}
}

void li_plugin_free(liServer *srv, liPlugin *p) {
	liServerState s;

	if (!p) return;

	s = g_atomic_int_get(&srv->state);
	if (LI_SERVER_INIT != s && LI_SERVER_DOWN != s) {
		ERROR(srv, "Cannot free plugin '%s' while server is running", p->name);
		return;
	}

	g_hash_table_remove(srv->plugins, p->name);
	li_plugin_free_default_options(srv, p);
	li_plugin_free_options(srv, p);
	li_plugin_free_actions(srv, p);
	li_plugin_free_setups(srv, p);
	if (p->free)
		p->free(srv, p);

	g_slice_free(liPlugin, p);
}

void li_server_plugins_free(liServer *srv) {
	gpointer key, val;
	GHashTableIter i;
	liServerState s;

	s = g_atomic_int_get(&srv->state);
	if (LI_SERVER_INIT != s && LI_SERVER_DOWN != s) {
		ERROR(srv, "%s", "Cannot free plugins while server is running");
		return;
	}

	g_hash_table_iter_init(&i, srv->plugins);
	while (g_hash_table_iter_next(&i, &key, &val)) {
		liPlugin *p = (liPlugin*) val;

		li_plugin_free_options(srv, p);
		li_plugin_free_actions(srv, p);
		li_plugin_free_setups(srv, p);
		if (p->free)
			p->free(srv, p);

		g_slice_free(liPlugin, p);
	}
	g_hash_table_destroy(srv->plugins);
	g_hash_table_destroy(srv->options);
	g_hash_table_destroy(srv->actions);
	g_hash_table_destroy(srv->setups);
}

liPlugin *li_plugin_register(liServer *srv, const gchar *name, liPluginInitCB init, gpointer userdata) {
	liPlugin *p;
	liServerState s;

	if (!init) {
		ERROR(srv, "Plugin '%s' needs an init function", name);
		return NULL;
	}

	s = g_atomic_int_get(&srv->state);
	if (LI_SERVER_INIT != s) {
		ERROR(srv, "Cannot register plugin '%s' after server was started", name);
		return NULL;
	}

	if (g_hash_table_lookup(srv->plugins, name)) {
		ERROR(srv, "Plugin '%s' already registered", name);
		return NULL;
	}

	p = plugin_new(name);
	p->id = g_hash_table_size(srv->plugins);
	g_hash_table_insert(srv->plugins, (gchar*) p->name, p);

	init(srv, p, userdata);
	p->opt_base_index = g_hash_table_size(srv->options);

	if (p->options) {
		size_t i;
		liServerOption *so;
		const liPluginOption *po;

		for (i = 0; (po = &p->options[i])->name; i++) {
			if (NULL != (so = (liServerOption*)g_hash_table_lookup(srv->options, po->name))) {
				ERROR(srv, "Option '%s' already registered by plugin '%s', unloading '%s'",
					po->name,
					so->p ? so->p->name : "<none>",
					p->name);
				li_plugin_free(srv, p);
				return NULL;
			}
			so = g_slice_new0(liServerOption);
			so->type = po->type;
			so->li_parse_option = po->li_parse_option;
			so->free_option = po->free_option;
			so->index = g_hash_table_size(srv->options);
			so->module_index = i;
			so->p = p;
			so->default_value = po->default_value;
			g_hash_table_insert(srv->options, (gchar*) po->name, so);
			plugin_load_default_option(srv, so);
		}
	}

	if (p->actions) {
		size_t i;
		liServerAction *sa;
		const liPluginAction *pa;

		for (i = 0; (pa = &p->actions[i])->name; i++) {
			if (NULL != (sa = (liServerAction*)g_hash_table_lookup(srv->actions, pa->name))) {
				ERROR(srv, "Action '%s' already registered by plugin '%s', unloading '%s'",
					pa->name,
					sa->p ? sa->p->name : "<none>",
					p->name);
				li_plugin_free(srv, p);
				return NULL;
			}
			sa = g_slice_new0(liServerAction);
			sa->li_create_action = pa->li_create_action;
			sa->p = p;
			sa->userdata = pa->userdata;
			g_hash_table_insert(srv->actions, (gchar*) pa->name, sa);
		}
	}

	if (p->setups) {
		size_t i;
		liServerSetup *ss;
		const liPluginSetup *ps;

		for (i = 0; (ps = &p->setups[i])->name; i++) {
			if (NULL != (ss = (liServerSetup*)g_hash_table_lookup(srv->setups, ps->name))) {
				ERROR(srv, "Setup '%s' already registered by plugin '%s', unloading '%s'",
					ps->name,
					ss->p ? ss->p->name : "<none>",
					p->name);
				li_plugin_free(srv, p);
				return NULL;
			}
			ss = g_slice_new0(liServerSetup);
			ss->setup = ps->setup;
			ss->p = p;
			ss->userdata = ps->userdata;
			g_hash_table_insert(srv->setups, (gchar*) ps->name, ss);
		}
	}

	return p;
}


static liServerOption* find_option(liServer *srv, const char *name) {
	return (liServerOption*) g_hash_table_lookup(srv->options, name);
}

gboolean li_parse_option(liServer *srv, const char *name, liValue *val, liOptionSet *mark) {
	liServerOption *sopt;

	if (!srv || !name || !mark) return FALSE;

	sopt = find_option(srv, name);
	if (!sopt) {
		ERROR(srv, "Unknown option '%s'", name);
		return FALSE;
	}

	if (sopt->type != val->type && sopt->type != LI_VALUE_NONE) {
		ERROR(srv, "Unexpected value type '%s', expected '%s' for option %s",
			li_value_type_string(val->type), li_value_type_string(sopt->type), name);
		return FALSE;
	}

	if (!sopt->li_parse_option) {
		mark->value = li_value_extract(val);
	} else {
		if (!sopt->li_parse_option(srv, sopt->p, sopt->module_index, val, &mark->value)) {
			/* errors should be logged by parse function */
			return FALSE;
		}
	}

	mark->ndx = sopt->index;
	mark->sopt = sopt;

	return TRUE;
}

void li_release_option(liServer *srv, liOptionSet *mark) { /** Does not free the option_set memory */
	liServerOption *sopt;
	if (!srv || !mark || !mark->sopt) return;
	sopt = mark->sopt;

	mark->sopt = NULL;
	if (!sopt->free_option) {
		switch (sopt->type) {
		case LI_VALUE_NONE:
		case LI_VALUE_BOOLEAN:
		case LI_VALUE_NUMBER:
			/* Nothing to free */
			break;
		case LI_VALUE_STRING:
			if (mark->value.string)
				g_string_free(mark->value.string, TRUE);
			break;
		case LI_VALUE_LIST:
			if (mark->value.list)
				li_value_list_free(mark->value.list);
			break;
		case LI_VALUE_HASH:
			if (mark->value.hash)
				g_hash_table_destroy(mark->value.hash);
			break;
		case LI_VALUE_ACTION:
			if (mark->value.action)
				li_action_release(srv, mark->value.action);
			break;
		case LI_VALUE_CONDITION:
			if (mark->value.cond)
				li_condition_release(srv, mark->value.cond);
			break;
		}
	} else {
		sopt->free_option(srv, sopt->p, sopt->module_index, mark->value);
	}
	{
		liOptionValue empty = {0};
		mark->value = empty;
	}
}

liAction* li_option_action(liServer *srv, const gchar *name, liValue *val) {
	liOptionSet setting;

	if (!li_parse_option(srv, name, val, &setting)) {
		return NULL;
	}

	return li_action_new_setting(setting);
}

liAction* li_create_action(liServer *srv, const gchar *name, liValue *val) {
	liAction *a;
	liServerAction *sa;

	if (NULL == (sa = (liServerAction*) g_hash_table_lookup(srv->actions, name))) {
		ERROR(srv, "Action '%s' doesn't exist", name);
		return NULL;
	}

	if (NULL == (a = sa->li_create_action(srv, sa->p, val, sa->userdata))) {
		ERROR(srv, "Action '%s' creation failed", name);
		return NULL;
	}

	return a;
}

gboolean li_call_setup(liServer *srv, const char *name, liValue *val) {
	liServerSetup *ss;

	if (NULL == (ss = (liServerSetup*) g_hash_table_lookup(srv->setups, name))) {
		ERROR(srv, "Setup function '%s' doesn't exist", name);
		return FALSE;
	}

	if (!ss->setup(srv, ss->p, val, ss->userdata)) {
		ERROR(srv, "Setup '%s' failed", name);
		return FALSE;
	}

	return TRUE;
}

void li_plugins_prepare_callbacks(liServer *srv) {
	GHashTableIter iter;
	liPlugin *p;
	gpointer v;

	g_hash_table_iter_init(&iter, srv->plugins);
	while (g_hash_table_iter_next(&iter, NULL, &v)) {
		p = (liPlugin*) v;
		if (p->handle_close)
			g_array_append_val(srv->li_plugins_handle_close, p);
		if (p->handle_vrclose)
			g_array_append_val(srv->li_plugins_handle_vrclose, p);
	}
}

void li_plugins_handle_close(liConnection *con) {
	GArray *a = con->srv->li_plugins_handle_close;
	guint i, len = a->len;
	for (i = 0; i < len; i++) {
		liPlugin *p = g_array_index(a, liPlugin*, i);
		p->handle_close(con, p);
	}
}

void li_plugins_handle_vrclose(liVRequest *vr) {
	GArray *a = vr->wrk->srv->li_plugins_handle_vrclose;
	guint i, len = a->len;
	for (i = 0; i < len; i++) {
		liPlugin *p = g_array_index(a, liPlugin*, i);
		p->handle_vrclose(vr, p);
	}
}

gboolean li_plugin_set_default_option(liServer *srv, const gchar* name, liValue *val) {
	liServerOption *sopt;
	liOptionSet setting;
	liOptionValue v;

	sopt = find_option(srv, name);

	if (!sopt) {
		ERROR(srv, "unknown option \"%s\"", name);
		return FALSE;
	}

	/* assign new value */
	if (!li_parse_option(srv, name, val, &setting)) {
		return FALSE;
	}

	v = g_array_index(srv->option_def_values, liOptionValue, sopt->index);
	g_array_index(srv->option_def_values, liOptionValue, sopt->index) = setting.value;

	/* free old value */
	setting.sopt = sopt;
	setting.ndx = sopt->index;
	setting.value = v;

	li_release_option(srv, &setting);

	return TRUE;
}

static gboolean plugin_load_default_option(liServer *srv, liServerOption *sopt) {
	liOptionValue oval = {0};

	if (!sopt)
		return FALSE;

	if (!sopt->li_parse_option) {
		switch (sopt->type) {
		case LI_VALUE_NONE:
			break;
		case LI_VALUE_BOOLEAN:
			oval.boolean = GPOINTER_TO_INT(sopt->default_value);
		case LI_VALUE_NUMBER:
			oval.number = GPOINTER_TO_INT(sopt->default_value);
			break;
		case LI_VALUE_STRING:
			oval.string = g_string_new((const char*) sopt->default_value);
			break;
		default:
			oval.ptr = NULL;
		}
	} else {
		if (!sopt->li_parse_option(srv, sopt->p, sopt->module_index, NULL, &oval)) {
			/* errors should be logged by parse function */
			return FALSE;
		}
	}

	if (srv->option_def_values->len <= sopt->index)
		g_array_set_size(srv->option_def_values, sopt->index + 1);

	g_array_index(srv->option_def_values, liOptionValue, sopt->index) = oval;

	return TRUE;
}

static void li_plugin_free_default_options(liServer *srv, liPlugin *p) {
	static const liOptionValue oempty = {0};
	GHashTableIter iter;
	gpointer k, v;

	g_hash_table_iter_init(&iter, srv->options);
	while (g_hash_table_iter_next(&iter, &k, &v)) {
		liServerOption *sopt = v;
		liOptionSet mark;
		mark.sopt = sopt;
		mark.ndx = sopt->index;

		if (sopt->p != p)
			continue;

		mark.value = g_array_index(srv->option_def_values, liOptionValue, sopt->index);

		li_release_option(srv, &mark);
		g_array_index(srv->option_def_values, liOptionValue, sopt->index) = oempty;
	}
}

void li_plugins_prepare_worker(liWorker *srv) { /* blocking callbacks */
	/* TODO */
}
void li_plugins_prepare(liServer* srv) { /* "prepare", async */
	/* TODO */
}

void li_plugins_start_listen(liServer *srv) { /* "warmup" */
	/* TODO */
}
void li_plugins_stop_listen(liServer *srv) { /* "prepare suspend", async */
	/* TODO */
}
void li_plugins_start_log(liServer *srv) { /* "run" */
	/* TODO */
}
void li_plugins_stop_log(liServer *srv) { /* "suspend now" */
	/* TODO */
}

void li_plugin_ready_for_state(liServer *srv, liPlugin *p, liServerState state) {
	/* TODO */
}

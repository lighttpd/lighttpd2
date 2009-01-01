
#include <lighttpd/base.h>

static gboolean plugin_load_default_option(server *srv, server_option *sopt);
static void plugin_free_default_options(server *srv, plugin *p);

static plugin* plugin_new(const gchar *name) {
	plugin *p = g_slice_new0(plugin);
	p->name = name;
	return p;
}

static void plugin_free_options(server *srv, plugin *p) {
	size_t i;
	const plugin_option *po;
	server_option *so;

	if (!p->options) return;
	for (i = 0; (po = &p->options[i])->name; i++) {
		if (NULL == (so = g_hash_table_lookup(srv->options, po->name))) break;
		if (so->p != p) break;
		g_hash_table_remove(srv->options, po->name);
	}
}

static void plugin_free_actions(server *srv, plugin *p) {
	size_t i;
	const plugin_action *pa;
	server_action *sa;

	if (!p->actions) return;
	for (i = 0; (pa = &p->actions[i])->name; i++) {
		if (NULL == (sa = g_hash_table_lookup(srv->actions, pa->name))) break;
		if (sa->p != p) break;
		g_hash_table_remove(srv->actions, pa->name);
	}
}

static void plugin_free_setups(server *srv, plugin *p) {
	size_t i;
	const plugin_setup *ps;
	server_setup *ss;

	if (!p->setups) return;
	for (i = 0; (ps = &p->setups[i])->name; i++) {
		if (NULL == (ss = g_hash_table_lookup(srv->setups, ps->name))) break;
		if (ss->p != p) break;
		g_hash_table_remove(srv->setups, ps->name);
	}
}

void plugin_free(server *srv, plugin *p) {
	if (!p) return;

	if (g_atomic_int_get(&srv->state) == SERVER_RUNNING) {
		ERROR(srv, "Cannot free plugin '%s' while server is running", p->name);
		return;
	}

	g_hash_table_remove(srv->plugins, p->name);
	plugin_free_default_options(srv, p);
	plugin_free_options(srv, p);
	plugin_free_actions(srv, p);
	plugin_free_setups(srv, p);
	if (p->free)
		p->free(srv, p);

	g_slice_free(plugin, p);
}

void server_plugins_free(server *srv) {
	gpointer key, val;
	GHashTableIter i;

	if (g_atomic_int_get(&srv->state) == SERVER_RUNNING) {
		ERROR(srv, "%s", "Cannot free plugins while server is running");
		return;
	}

	g_hash_table_iter_init(&i, srv->plugins);
	while (g_hash_table_iter_next(&i, &key, &val)) {
		plugin *p = (plugin*) val;

		plugin_free_options(srv, p);
		plugin_free_actions(srv, p);
		plugin_free_setups(srv, p);

		g_slice_free(plugin, p);
	}
	g_hash_table_destroy(srv->plugins);
	g_hash_table_destroy(srv->options);
	g_hash_table_destroy(srv->actions);
	g_hash_table_destroy(srv->setups);
}

plugin *plugin_register(server *srv, const gchar *name, PluginInit init) {
	plugin *p;

	if (!init) {
		ERROR(srv, "Module '%s' needs an init function", name);
		return NULL;
	}

	if (g_atomic_int_get(&srv->state) != SERVER_STARTING) {
		ERROR(srv, "Cannot register plugin '%s' after server was started", name);
		return NULL;
	}

	if (g_hash_table_lookup(srv->plugins, name)) {
		ERROR(srv, "Module '%s' already registered", name);
		return NULL;
	}

	p = plugin_new(name);
	p->id = g_hash_table_size(srv->plugins);
	g_hash_table_insert(srv->plugins, (gchar*) p->name, p);

	init(srv, p);
	p->opt_base_index = g_hash_table_size(srv->options);

	if (p->options) {
		size_t i;
		server_option *so;
		const plugin_option *po;

		for (i = 0; (po = &p->options[i])->name; i++) {
			if (NULL != (so = (server_option*)g_hash_table_lookup(srv->options, po->name))) {
				ERROR(srv, "Option '%s' already registered by plugin '%s', unloading '%s'",
					po->name,
					so->p ? so->p->name : "<none>",
					p->name);
				plugin_free(srv, p);
				return NULL;
			}
			so = g_slice_new0(server_option);
			so->type = po->type;
			so->parse_option = po->parse_option;
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
		server_action *sa;
		const plugin_action *pa;

		for (i = 0; (pa = &p->actions[i])->name; i++) {
			if (NULL != (sa = (server_action*)g_hash_table_lookup(srv->actions, pa->name))) {
				ERROR(srv, "Action '%s' already registered by plugin '%s', unloading '%s'",
					pa->name,
					sa->p ? sa->p->name : "<none>",
					p->name);
				plugin_free(srv, p);
				return NULL;
			}
			sa = g_slice_new0(server_action);
			sa->create_action = pa->create_action;
			sa->p = p;
			g_hash_table_insert(srv->actions, (gchar*) pa->name, sa);
		}
	}

	if (p->setups) {
		size_t i;
		server_setup *ss;
		const plugin_setup *ps;

		for (i = 0; (ps = &p->setups[i])->name; i++) {
			if (NULL != (ss = (server_setup*)g_hash_table_lookup(srv->setups, ps->name))) {
				ERROR(srv, "Setup '%s' already registered by plugin '%s', unloading '%s'",
					ps->name,
					ss->p ? ss->p->name : "<none>",
					p->name);
				plugin_free(srv, p);
				return NULL;
			}
			ss = g_slice_new0(server_setup);
			ss->setup = ps->setup;
			ss->p = p;
			g_hash_table_insert(srv->setups, (gchar*) ps->name, ss);
		}
	}

	return p;
}


static server_option* find_option(server *srv, const char *name) {
	return (server_option*) g_hash_table_lookup(srv->options, name);
}

gboolean parse_option(server *srv, const char *name, value *val, option_set *mark) {
	server_option *sopt;

	if (!srv || !name || !mark) return FALSE;

	sopt = find_option(srv, name);
	if (!sopt) {
		ERROR(srv, "Unknown option '%s'", name);
		return FALSE;
	}

	if (sopt->type != val->type && sopt->type != VALUE_NONE) {
		ERROR(srv, "Unexpected value type '%s', expected '%s' for option %s",
			value_type_string(val->type), value_type_string(sopt->type), name);
		return FALSE;
	}

	if (!sopt->parse_option) {
		mark->value = value_extract(val);
	} else {
		if (!sopt->parse_option(srv, sopt->p, sopt->module_index, val, &mark->value)) {
			/* errors should be logged by parse function */
			return FALSE;
		}
	}

	mark->ndx = sopt->index;
	mark->sopt = sopt;

	return TRUE;
}

void release_option(server *srv, option_set *mark) { /** Does not free the option_set memory */
	server_option *sopt;
	if (!srv || !mark || !mark->sopt) return;
	sopt = mark->sopt;

	mark->sopt = NULL;
	if (!sopt->free_option) {
		switch (sopt->type) {
		case VALUE_NONE:
		case VALUE_BOOLEAN:
		case VALUE_NUMBER:
			/* Nothing to free */
			break;
		case VALUE_STRING:
			if (mark->value.string)
				g_string_free(mark->value.string, TRUE);
			break;
		case VALUE_LIST:
			if (mark->value.list)
				value_list_free(mark->value.list);
			break;
		case VALUE_HASH:
			if (mark->value.hash)
				g_hash_table_destroy(mark->value.hash);
			break;
		case VALUE_ACTION:
			if (mark->value.action)
				action_release(srv, mark->value.action);
			break;
		case VALUE_CONDITION:
			if (mark->value.cond)
				condition_release(srv, mark->value.cond);
			break;
		}
	} else {
		sopt->free_option(srv, sopt->p, sopt->module_index, mark->value);
	}
	{
		option_value empty = {0};
		mark->value = empty;
	}
}

action* option_action(server *srv, const gchar *name, value *val) {
	option_set setting;

	if (!parse_option(srv, name, val, &setting)) {
		return NULL;
	}

	return action_new_setting(setting);
}

action* create_action(server *srv, const gchar *name, value *val) {
	action *a;
	server_action *sa;

	if (NULL == (sa = (server_action*) g_hash_table_lookup(srv->actions, name))) {
		ERROR(srv, "Action '%s' doesn't exist", name);
		return NULL;
	}

	if (NULL == (a = sa->create_action(srv, sa->p, val))) {
		ERROR(srv, "Action '%s' creation failed", name);
		return NULL;
	}

	return a;
}

gboolean call_setup(server *srv, const char *name, value *val) {
	server_setup *ss;

	if (NULL == (ss = (server_setup*) g_hash_table_lookup(srv->setups, name))) {
		ERROR(srv, "Setup function '%s' doesn't exist", name);
		return FALSE;
	}

	if (!ss->setup(srv, ss->p, val)) {
		ERROR(srv, "Setup '%s' failed", name);
		return FALSE;
	}

	return TRUE;
}

void plugins_prepare_callbacks(server *srv) {
	GHashTableIter iter;
	plugin *p;
	gpointer v;

	g_hash_table_iter_init(&iter, srv->plugins);
	while (g_hash_table_iter_next(&iter, NULL, &v)) {
		p = (plugin*) v;
		if (p->handle_close)
			g_array_append_val(srv->plugins_handle_close, p);
		if (p->handle_vrclose)
			g_array_append_val(srv->plugins_handle_vrclose, p);
	}
}

void plugins_handle_close(connection *con) {
	GArray *a = con->srv->plugins_handle_close;
	guint i, len = a->len;
	for (i = 0; i < len; i++) {
		plugin *p = g_array_index(a, plugin*, i);
		p->handle_close(con, p);
	}
}

void plugins_handle_vrclose(vrequest *vr) {
	GArray *a = vr->con->srv->plugins_handle_vrclose;
	guint i, len = a->len;
	for (i = 0; i < len; i++) {
		plugin *p = g_array_index(a, plugin*, i);
		p->handle_vrclose(vr, p);
	}
}

gboolean plugin_set_default_option(server *srv, const gchar* name, value *val) {
	server_option *sopt;
	option_set setting;
	option_value v;

	sopt = find_option(srv, name);

	if (!sopt) {
		ERROR(srv, "unknown option \"%s\"", name);
		return FALSE;
	}

	/* assign new value */
	if (!parse_option(srv, name, val, &setting)) {
		return FALSE;
	}

	v = g_array_index(srv->option_def_values, option_value, sopt->index);
	g_array_index(srv->option_def_values, option_value, sopt->index) = setting.value;

	/* free old value */
	setting.sopt = sopt;
	setting.ndx = sopt->index;
	setting.value = v;

	release_option(srv, &setting);

	return TRUE;
}

static gboolean plugin_load_default_option(server *srv, server_option *sopt) {
	option_value oval = {0};

	if (!sopt)
		return FALSE;

	if (!sopt->parse_option) {
		switch (sopt->type) {
		case VALUE_NONE:
			break;
		case VALUE_BOOLEAN:
			oval.boolean = GPOINTER_TO_INT(sopt->default_value);
		case VALUE_NUMBER:
			oval.number = GPOINTER_TO_INT(sopt->default_value);
			break;
		case VALUE_STRING:
			oval.string = g_string_new((const char*) sopt->default_value);
			break;
		default:
			oval.ptr = NULL;
		}
	} else {
		if (!sopt->parse_option(srv, sopt->p, sopt->module_index, NULL, &oval)) {
			/* errors should be logged by parse function */
			return FALSE;
		}
	}

	if (srv->option_def_values->len <= sopt->index)
		g_array_set_size(srv->option_def_values, sopt->index + 1);

	g_array_index(srv->option_def_values, option_value, sopt->index) = oval;

	return TRUE;
}

static void plugin_free_default_options(server *srv, plugin *p) {
	static const option_value oempty = {0};
	GHashTableIter iter;
	gpointer k, v;

	g_hash_table_iter_init(&iter, srv->options);
	while (g_hash_table_iter_next(&iter, &k, &v)) {
		server_option *sopt = v;
		option_set mark;
		mark.sopt = sopt;
		mark.ndx = sopt->index;

		if (sopt->p != p)
			continue;

		mark.value = g_array_index(srv->option_def_values, option_value, sopt->index);

		release_option(srv, &mark);
		g_array_index(srv->option_def_values, option_value, sopt->index) = oempty;
	}
}

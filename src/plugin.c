
#include "plugin.h"
#include "log.h"

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

	g_hash_table_remove(srv->plugins, p->name);
	plugin_free_options(srv, p);
	plugin_free_actions(srv, p);
	plugin_free_setups(srv, p);

	g_slice_free(plugin, p);
}

gboolean plugin_register(server *srv, const gchar *name, PluginInit init) {
	plugin *p;

	if (!init) {
		ERROR(srv, "Module '%s' needs an init function", name);
		return FALSE;
	}

	if (g_hash_table_lookup(srv->plugins, name)) {
		ERROR(srv, "Module '%s' already registered", name);
		return FALSE;
	}

	p = plugin_new(name);
	g_hash_table_insert(srv->plugins, (gchar*) p->name, p);

	init(srv, p);

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
				return FALSE;
			}
			so = g_slice_new0(server_option);
			so->type = po->type;
			so->parse_option = po->parse_option;
			so->free_option = po->free_option;
			so->index = g_hash_table_size(srv->options);
			so->module_index = i;
			so->p = p;
			g_hash_table_insert(srv->options, (gchar*) po->name, so);
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
				return FALSE;
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
				return FALSE;
			}
			ss = g_slice_new0(server_setup);
			ss->setup = ps->setup;
			ss->p = p;
			g_hash_table_insert(srv->setups, (gchar*) ps->name, ss);
		}
	}

	return TRUE;
}


static server_option* find_option(server *srv, const char *name) {
	return (server_option*) g_hash_table_lookup(srv->options, name);
}

gboolean parse_option(server *srv, const char *name, option *opt, option_set *mark) {
	server_option *sopt;

	if (!srv || !name || !mark) return FALSE;

	sopt = find_option(srv, name);
	if (!sopt) {
		ERROR(srv, "Unknown option '%s'", name);
		return FALSE;
	}

	if (sopt->type != opt->type) {
		ERROR(srv, "Unexpected option type '%s', expected '%s'",
			option_type_string(opt->type), option_type_string(sopt->type));
		return FALSE;
	}

	if (!sopt->parse_option) {
		mark->value = option_extract_value(opt);
	} else {
		if (!sopt->parse_option(srv, sopt->p, sopt->module_index, opt, &mark->value)) {
			/* errors should be logged by parse function */
			return FALSE;
		}
	}

	mark->ndx = sopt->index;
	mark->sopt = sopt;

	return TRUE;
}

void release_option(server *srv, option_set *mark) { /** Does not free the option_set memory */
	server_option *sopt = mark->sopt;
	if (!srv || !mark || !sopt) return;

	mark->sopt = NULL;
	if (!sopt->free_option) {
		switch (sopt->type) {
		case OPTION_NONE:
		case OPTION_BOOLEAN:
		case OPTION_INT:
			/* Nothing to free */
			break;
		case OPTION_STRING:
			g_string_free((GString*) mark->value, TRUE);
			break;
		case OPTION_LIST:
			option_list_free((GArray*) mark->value);
			break;
		case OPTION_HASH:
			g_hash_table_destroy((GHashTable*) mark->value);
			break;
		case OPTION_ACTION:
			action_release(srv, (action*) mark->value);
			break;
		case OPTION_CONDITION:
			condition_release(srv, (condition*) mark->value);
			break;
		}
	} else {
		sopt->free_option(srv, sopt->p, sopt->module_index, mark->value);
	}
	mark->value = NULL;
}

action* create_action(server *srv, const gchar *name, option *value) {
	action *a;
	server_action *sa;

	if (NULL == (sa = (server_action*) g_hash_table_lookup(srv->actions, name))) {
		ERROR(srv, "Action '%s' doesn't exist", name);
		return NULL;
	}

	if (NULL == (a = sa->create_action(srv, sa->p, value))) {
		ERROR(srv, "Action '%s' creation failed", name);
		return NULL;
	}

	return a;
}

gboolean call_setup(server *srv, const char *name, option *opt) {
	server_setup *ss;

	if (NULL == (ss = (server_setup*) g_hash_table_lookup(srv->actions, name))) {
		ERROR(srv, "Setup function '%s' doesn't exist", name);
		return FALSE;
	}

	if (!ss->setup(srv, ss->p, opt)) {
		ERROR(srv, "Setup '%s' failed", name);
		return FALSE;
	}

	return TRUE;
}

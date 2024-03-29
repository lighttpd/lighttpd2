
#include <lighttpd/angel_base.h>
#include <lighttpd/angel_config_parser.h>
#include <lighttpd/angel_plugin_core.h>

/* internal structures */

typedef struct server_item server_item;
struct server_item {
	liPlugin *p;
	const liPluginItem *p_item;
};

typedef struct server_module server_module;
struct server_module {
	gchar *name;
	liServer *srv;
	liModule *mod;
	GPtrArray *plugins; /* plugin* */
};

static void _server_item_free(gpointer p) {
	g_slice_free(server_item, p);
}

static server_item* server_item_new(liPlugin *p, const liPluginItem *p_item) {
	server_item *si = g_slice_new(server_item);
	si->p = p;
	si->p_item = p_item;
	return si;
}

static void li_plugin_free(liServer *srv, liPlugin *p) {
	if (p->handle_free) p->handle_free(srv, p);
	g_hash_table_destroy(p->angel_callbacks);
	g_slice_free(liPlugin, p);
}

static liPlugin* plugin_new(const char *name) {
	liPlugin *p = g_slice_new0(liPlugin);
	p->name = name;
	p->angel_callbacks = g_hash_table_new(g_str_hash, g_str_equal);
	return p;
}

static void _server_module_release(gpointer d) {
	server_module *sm = d;
	guint i;

	for (i = sm->plugins->len; i-- > 0; ) {
		liPlugin *p = g_ptr_array_index(sm->plugins, i);
		li_plugin_free(sm->srv, p);
	}
	g_ptr_array_free(sm->plugins, TRUE);
	if (sm->mod) li_module_release(sm->srv->plugins.modules, sm->mod);
	g_free(sm->name);
	g_slice_free(server_module, sm);
}

static server_module* server_module_new(liServer *srv, const gchar *name) { /* module is set later */
	server_module *sm = g_slice_new0(server_module);
	sm->srv = srv;
	sm->plugins = g_ptr_array_new();
	sm->name = g_strdup(name);
	return sm;
}

void li_plugins_init(liServer *srv, const gchar *module_dir, gboolean module_resident) {
	liPlugins *ps = &srv->plugins;

	ps->modules = li_modules_new(srv, module_dir, module_resident);

	ps->items = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, _server_item_free);

	ps->module_refs = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, _server_module_release);

	ps->ht_plugins = g_hash_table_new(g_str_hash, g_str_equal);

	ps->plugins = g_ptr_array_new();
}

void li_plugins_clear(liServer *srv) {
	liPlugins *ps = &srv->plugins;

	g_hash_table_destroy(ps->items);
	g_hash_table_destroy(ps->module_refs);
	g_hash_table_destroy(ps->ht_plugins);
	g_ptr_array_free(ps->plugins, TRUE);

	if (ps->config_filename) g_string_free(ps->config_filename, TRUE);

	li_modules_free(ps->modules);
}

gboolean li_plugins_config_load(liServer *srv, const gchar *filename) {
	liPlugins *ps = &srv->plugins;
	GError *error = NULL;
	guint i;

	if (!li_plugins_load_module(srv, NULL)) {
		ERROR(srv, "%s", "failed loading core plugins");
		return FALSE;
	}

	if (!li_angel_config_parse_file(srv, filename, &error)) {
		ERROR(srv, "failed to parse config file: %s", error->message);
		g_error_free(error);
		return FALSE;
	}

	DEBUG(srv, "parsed config file: %s", filename);

	/* check new config */
	for (i = ps->plugins->len; i-- > 0; ) {
		liPlugin *p = g_ptr_array_index(ps->plugins, i);
		if (p->handle_check_config) {
			if (!p->handle_check_config(srv, p, &error)) {
				ERROR(srv, "config check failed: %s", error->message);
				g_error_free(error);
				return FALSE;
			}
		}
	}

	/* activate new config */
	for (i = ps->plugins->len; i-- > 0; ) {
		liPlugin *p = g_ptr_array_index(ps->plugins, i);
		DEBUG(srv, "activate plugin: %s", p->name);
		if (p->handle_activate_config) {
			p->handle_activate_config(srv, p);
		}
	}

	DEBUG(srv, "%s", "config loading done");

	LI_FORCE_ASSERT(NULL == ps->config_filename);
	ps->config_filename = g_string_new(filename);

	return TRUE;
}

void li_plugins_stop(liServer *srv) {
	liPlugins *ps = &srv->plugins;
	guint i;

	g_string_assign(ps->config_filename, "");

	/* activate new config */
	for (i = ps->plugins->len; i-- > 0; ) {
		liPlugin *p = g_ptr_array_index(ps->plugins, i);
		INFO(srv, "stop: %s", p->name);
		if (p->handle_stop) {
			p->handle_stop(srv, p);
		}
	}
}

gboolean li_plugins_handle_item(liServer *srv, GString *itemname, liValue *parameters, GError **err) {
	liPlugins *ps = &srv->plugins;
	server_item *si;

#if 0
	/* debug items */
	{
		GString *tmp = li_value_to_string(parameters);
		ERROR(srv, "Item '%s': %s", itemname->str, tmp->str);
		g_string_free(tmp, TRUE);
	}
#endif

	si = g_hash_table_lookup(ps->items, itemname->str);
	if (!si) {
		g_set_error(err, LI_ANGEL_CONFIG_PARSER_ERROR, LI_ANGEL_CONFIG_PARSER_ERROR_PARSE,
			"Unknown item '%s' - perhaps you forgot to load the module?", itemname->str);
		return FALSE;
	} else {
		return si->p_item->handle_parse_item(srv, si->p, parameters, err);
	}
}

static gboolean plugins_activate_module(liServer *srv, server_module *sm) {
	liPlugins *ps = &srv->plugins;
	liPlugin *p;
	const liPluginItem *pi;
	guint i;

	for (i = 0; i < sm->plugins->len; i++) {
		p = g_ptr_array_index(sm->plugins, i);
		g_ptr_array_add(ps->plugins, p);
		g_hash_table_insert(ps->ht_plugins, (gpointer) p->name, p);
		if (!p->items) continue;

		for (pi = p->items; pi->name; pi++) {
			server_item *si;
			if (NULL != (si = g_hash_table_lookup(ps->items, pi->name))) {
				ERROR(srv, "Plugin item name conflict: cannot load '%s' for plugin '%s' (already provided by plugin '%s')",
					pi->name, p->name, si->p->name);
				goto item_collision;
			} else {
				si = server_item_new(p, pi);
				g_hash_table_insert(ps->items, (gpointer) pi->name, si);
			}
		}
	}

	return TRUE;

item_collision:
	/* removed added items and plugins */
	for ( ; pi-- != p->items; ) {
		g_hash_table_remove(ps->items, pi->name);
	}

	g_ptr_array_set_size(ps->plugins, ps->plugins->len - i+1);

	for ( ; i-- > 0; ) {
		p = g_ptr_array_index(sm->plugins, i);
		g_hash_table_remove(ps->ht_plugins, p->name);
		if (!p->items) continue;

		for (pi = p->items; pi->name; pi++) {
			g_hash_table_remove(ps->items, pi->name);
		}
	}

	return FALSE;
}

gboolean li_plugins_load_module(liServer *srv, const gchar *name) {
	liPlugins *ps = &srv->plugins;
	server_module *sm;
	const gchar* modname = name ? name : "core";
	GError *err = NULL;

	sm = g_hash_table_lookup(ps->module_refs, modname);
	if (sm) return TRUE; /* already loaded */

	{
		/* not loaded yet */
		liModule *mod;
		sm = server_module_new(srv, modname);
		g_hash_table_insert(ps->module_refs, sm->name, sm);
		if (name) {
			mod = li_module_load(ps->modules, name, &err);

			if (!mod) {
				ERROR(srv, "Couldn't load dependency '%s': %s", name, err->message);
				g_error_free(err);
				_server_module_release(sm);
				return FALSE;
			}
			sm->mod = mod;
		} else {
			if (!li_plugin_core_init(srv)) {
				_server_module_release(sm);
				return FALSE;
			}
		}
	}

	if (!plugins_activate_module(srv, sm)) {
		_server_module_release(sm);
		return FALSE;
	}

	return TRUE;
}

liPlugin *li_angel_plugin_register(liServer *srv, liModule *mod, const gchar *name, liPluginInitCB init) {
	liPlugins *ps = &srv->plugins;
	server_module *sm;
	liPlugin *p;
	const gchar* modname = mod ? mod->name->str : "core";

	sm = g_hash_table_lookup(ps->module_refs, modname);
	if (!sm) {
		ERROR(srv, "Module '%s' not loaded; cannot load plugin '%s'", modname, name);
		return NULL;
	}

	p = plugin_new(name);
	if (!init(srv, p)) {
		ERROR(srv, "Couldn't load plugin '%s' for module '%s': init failed", name, modname);
		li_plugin_free(srv, p);
		return NULL;
	}

	g_ptr_array_add(sm->plugins, p);

	return p;
}

void li_angel_plugin_replaced_instance(liServer *srv, liInstance *oldi, liInstance *newi) {
	liPlugins *ps = &srv->plugins;
	guint i;

	for (i = 0; i < ps->plugins->len; i++) {
		liPlugin *p = g_ptr_array_index(ps->plugins, i);
		if (p->handle_instance_replaced) p->handle_instance_replaced(srv, p, oldi, newi);
	}
}

void li_angel_plugin_instance_reached_state(liServer *srv, liInstance *inst, liInstanceState s) {
	liPlugins *ps = &srv->plugins;
	guint i;

	for (i = 0; i < ps->plugins->len; i++) {
		liPlugin *p = g_ptr_array_index(ps->plugins, i);
		if (p->handle_instance_reached_state) p->handle_instance_reached_state(srv, p, inst, s);
	}
}

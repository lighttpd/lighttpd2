
#include <lighttpd/angel_base.h>
#include <lighttpd/angel_config_parser.h>
#include <lighttpd/angel_plugin_core.h>

/* internal structures */

typedef struct server_item server_item;
struct server_item {
	plugin *p;
	guint option_count;
	const plugin_item *p_item;
};

typedef struct server_module server_module;
struct server_module {
	guint refcount;
	gchar *name;
	server *srv;
	module *mod;
	GPtrArray *plugins; /* plugin* */
};

static void _server_item_free(gpointer p) {
	g_slice_free(server_item, p);
}

static server_item* server_item_new(plugin *p, const plugin_item *p_item) {
	server_item *si = g_slice_new(server_item);
	const plugin_item_option *pio;
	guint cnt;
	for (pio = p_item->options, cnt = 0; pio->name; pio++, cnt++) ;
	si->p = p;
	si->option_count = cnt;
	si->p_item = p_item;
	return si;
}

static void plugin_free(server *srv, plugin *p) {
	if (p->handle_free) p->handle_free(srv, p);
	g_slice_free(plugin, p);
}

static plugin* plugin_new(const char *name) {
	plugin *p = g_slice_new0(plugin);
	p->name = name;
	return p;
}

static void _server_module_release(gpointer d) {
	server_module *sm = d;
	guint i;

	g_assert(sm->refcount > 0);
	if (0 != --sm->refcount) return;

	for (i = sm->plugins->len; i-- > 0; ) {
		plugin *p = g_ptr_array_index(sm->plugins, i);
		plugin_free(sm->srv, p);
	}
	g_ptr_array_free(sm->plugins, TRUE);
	if (sm->mod) module_release(sm->srv->plugins.modules, sm->mod);
	g_free(sm->name);
	g_slice_free(server_module, sm);
}

static void server_module_acquire(server_module *sm) {
	g_assert(sm->refcount > 0);
	sm->refcount++;
}

static server_module* server_module_new(server *srv, const gchar *name) { /* module is set later */
	server_module *sm = g_slice_new0(server_module);
	sm->refcount = 1;
	sm->srv = srv;
	sm->plugins = g_ptr_array_new();
	sm->name = g_strdup(name);
	return sm;
}

void plugins_init(server *srv, const gchar *module_dir) {
	Plugins *ps = &srv->plugins;

	ps->modules = modules_new(srv, module_dir);

	ps->items = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, _server_item_free);
	ps->load_items = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, _server_item_free);

	ps->module_refs = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, _server_module_release);
	ps->load_module_refs = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, _server_module_release);

	ps->plugins = g_ptr_array_new();
	ps->load_plugins = g_ptr_array_new();
}

void plugins_clear(server *srv) {
	Plugins *ps = &srv->plugins;

	plugins_config_clean(srv);

	g_hash_table_destroy(ps->items);
	g_hash_table_destroy(ps->load_items);

	g_hash_table_destroy(ps->module_refs);
	g_hash_table_destroy(ps->load_module_refs);

	g_ptr_array_free(ps->plugins, TRUE);
	g_ptr_array_free(ps->load_plugins, TRUE);

	if (ps->config_filename) g_string_free(ps->config_filename, TRUE);

	modules_free(ps->modules);
}

void plugins_config_clean(server *srv) {
	Plugins *ps = &srv->plugins;
	guint i;

	for (i = ps->load_plugins->len; i-- > 0; ) {
		plugin *p = g_ptr_array_index(ps->load_plugins, i);
		if (p->handle_clean_config) p->handle_clean_config(srv, p);
	}

	g_hash_table_remove_all(ps->load_items);
	g_hash_table_remove_all(ps->load_module_refs);
	g_ptr_array_set_size(ps->load_plugins, 0);
}

gboolean plugins_config_load(server *srv, const gchar *filename) {
	Plugins *ps = &srv->plugins;
	GError *error = NULL;
	guint i;

	if (!angel_config_parse_file(srv, filename, &error)) {
		ERROR(srv, "failed to parse config file: %s\n", error->message);
		g_error_free(error);
		plugins_config_clean(srv);
		return FALSE;
	}

	/* check new config */
	for (i = ps->plugins->len; i-- > 0; ) {
		plugin *p = g_ptr_array_index(ps->load_plugins, i);
		if (p->handle_check_config) {
			if (!p->handle_check_config(srv, p)) {
				plugins_config_clean(srv);
				return FALSE;
			}
		}
	}

	/* activate new config */
	for (i = ps->plugins->len; i-- > 0; ) {
		plugin *p = g_ptr_array_index(ps->load_plugins, i);
		if (p->handle_activate_config) {
			p->handle_activate_config(srv, p);
		}
	}

	{ /* swap the arrays */
		GPtrArray *tmp = ps->load_plugins; ps->load_plugins = ps->plugins; ps->plugins = tmp;
	}
	{ /* swap the hash tables */
		GHashTable *tmp;
		tmp = ps->load_items; ps->load_items = ps->items; ps->items = tmp;
		tmp = ps->load_module_refs; ps->load_module_refs = ps->module_refs; ps->module_refs = tmp;
	}
	g_hash_table_remove_all(ps->load_items);
	g_hash_table_remove_all(ps->load_module_refs);
	g_ptr_array_set_size(ps->load_plugins, 0);

	if (!ps->config_filename) {
		ps->config_filename = g_string_new(filename);
	} else {
		g_string_assign(ps->config_filename, filename);
	}

	return TRUE;
}

gboolean plugins_handle_item(server *srv, GString *itemname, value *hash) {
	Plugins *ps = &srv->plugins;
	server_item *si;

#if 1
	/* debug items */
	{
		GString *tmp = value_to_string(hash);
		ERROR(srv, "Item '%s': %s\n", itemname->str, tmp->str);
		g_string_free(tmp, TRUE);
	}
#endif

	si = g_hash_table_lookup(ps->load_items, itemname->str);
	if (!si) {
		WARNING(srv, "Unknown item '%s' - perhaps you forgot to load the module? (ignored)", itemname->str);
	} else {
		value **optlist = g_slice_alloc0(sizeof(value*) * si->option_count);
		GHashTableIter opti;
		gpointer k, v;
		guint i;
		gboolean valid = TRUE;

		/* find options and assign them by id */
		g_hash_table_iter_init(&opti, hash->data.hash);
		while (g_hash_table_iter_next(&opti, &k, &v)) {
			const gchar *optkey = ((GString*) k)->str;
			for (i = 0; i < si->option_count; i++) {
				if (0 == g_strcmp0(si->p_item->options[i].name, optkey)) break;
			}
			if (i == si->option_count) {
				WARNING(srv, "Unknown option '%s' in item '%s' (ignored)", optkey, itemname->str);
			} else {
				optlist[i] = v;
			}
		}

		/* validate options */
		for (i = 0; i < si->option_count; i++) {
			const plugin_item_option *pi = &si->p_item->options[i];
			if (0 != (pi->flags & PLUGIN_ITEM_OPTION_MANDATORY)) {
				if (!optlist[i]) {
					ERROR(srv, "Missing mandatory option '%s' in item '%s'", pi->name, itemname->str);
					valid = FALSE;
				}
			}
			if (pi->type != VALUE_NONE && optlist[i] && optlist[i]->type != pi->type) {
				/* TODO: convert from string if possible */
				ERROR(srv, "Invalid value type of option '%s' in item '%s', got '%s' but expected '%s'",
					pi->name, itemname->str, value_type_string(optlist[i]->type), value_type_string(pi->type));
				valid = FALSE;
			}
		}

		if (valid) {
			g_assert(si->p_item->handle_parse_item);
			si->p_item->handle_parse_item(srv, si->p, optlist);
		}

		g_slice_free1(sizeof(value*) * si->option_count, optlist);
	}
	return TRUE;
}

static gboolean plugins_activate_module(server *srv, server_module *sm) {
	Plugins *ps = &srv->plugins;
	plugin *p;
	const plugin_item *pi;
	guint i;

	for (i = 0; i < sm->plugins->len; i++) {
		p = g_ptr_array_index(sm->plugins, i);
		g_ptr_array_add(ps->load_plugins, p);
		if (!p->items) continue;

		for (pi = p->items; pi->name; pi++) {
			server_item *si;
			if (NULL != (si = g_hash_table_lookup(ps->load_items, pi->name))) {
				ERROR(srv, "Plugin item name conflict: cannot load '%s' for plugin '%s' (already provided by plugin '%s')",
					pi->name, p->name, si->p->name);
				goto item_collission;
			} else {
				si = server_item_new(p, pi);
				g_hash_table_insert(ps->load_items, (gpointer) pi->name, si);
			}
		}
	}

	return TRUE;

item_collission:
	/* removed added items and plugins */
	for ( ; pi-- != p->items; ) {
		g_hash_table_remove(ps->load_items, pi->name);
	}

	g_ptr_array_set_size(ps->load_plugins, ps->load_plugins->len - i+1);

	for ( ; i-- > 0; ) {
		p = g_ptr_array_index(sm->plugins, i);
		if (!p->items) continue;

		for (pi = p->items; pi->name; pi++) {
			g_hash_table_remove(ps->load_items, pi->name);
		}
	}

	return FALSE;
}

gboolean plugins_load_module(server *srv, const gchar *name) {
	Plugins *ps = &srv->plugins;
	server_module *sm;
	const gchar* modname = name ? name : "core";

	sm = g_hash_table_lookup(ps->load_module_refs, modname);
	if (sm) return TRUE; /* already loaded */
	sm = g_hash_table_lookup(ps->module_refs, modname);
	if (sm) { /* loaded in previous config */
		server_module_acquire(sm);
		g_hash_table_insert(ps->load_module_refs, sm->name, sm);
	} else { /* not loaded yet */
		module *mod;
		sm = server_module_new(srv, modname);
		g_hash_table_insert(ps->load_module_refs, sm->name, sm);
		if (name) {
			mod = module_load(ps->modules, name);

			if (!mod) {
				_server_module_release(sm);
				return FALSE;
			}
			sm->mod = mod;
		} else {
			if (!plugin_core_init(srv)) {
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

plugin *angel_plugin_register(server *srv, module *mod, const gchar *name, PluginInit init) {
	Plugins *ps = &srv->plugins;
	server_module *sm;
	plugin *p;
	const gchar* modname = mod ? mod->name->str : "core";

	sm = g_hash_table_lookup(ps->load_module_refs, modname);
	if (!sm) {
		ERROR(srv, "Module '%s' not loaded; cannot load plugin '%s'", mod->name->str, name);
		return NULL;
	}

	p = plugin_new(name);
	if (!init(srv, p)) {
		plugin_free(srv, p);
		return NULL;
	}

	g_ptr_array_add(sm->plugins, p);

	return p;
}

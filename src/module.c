
#include <lighttpd/module.h>

modules *modules_init(gpointer main, const gchar *module_dir) {
	modules *m = g_slice_new(modules);

	m->version = MODULE_VERSION;
	m->main = main;
	m->mods = g_array_new(FALSE, TRUE, sizeof(module*));
	m->module_dir = g_strdup(module_dir);
	m->sizeof_off_t = sizeof(off_t);

	return m;
}

module *module_lookup(modules *mods, const gchar *name) {
	module *mod;
	GArray *a = mods->mods;

	for (guint i = 0; i < a->len; i++) {
		mod = g_array_index(a, module*, i);
		if (mod != NULL && g_str_equal(mod->name->str, name))
			return mod;
	}

	return NULL;
}

void modules_cleanup(modules* mods) {
	/* unload all modules */
	GArray *a = mods->mods;
	module *mod;

	for (guint i = 0; i < a->len; i++) {
		mod = g_array_index(a, module*, i);
		if (!mod)
			continue;
		module_release(mods, mod);
	}

	g_array_free(mods->mods, TRUE);
	g_free(mods->module_dir);
	g_slice_free(modules, mods);
}


module* module_load(modules *mods, const gchar* name) {
	module *mod;
	ModuleInit m_init;
	GString *m_init_str, *m_free_str;
	guint i;

	mod = module_lookup(mods, name);

	if (mod) {
		/* module already loaded, increment refcount and return */
		mod->refcount++;
		return mod;
	}

	mod = g_slice_new0(module);
	mod->name = g_string_new(name);
	mod->refcount = 1;
	mod->path = g_module_build_path(mods->module_dir, name);

	mod->module = g_module_open(mod->path, G_MODULE_BIND_LAZY);

	if (!mod->module) {
		g_string_free(mod->name, TRUE);
		g_free(mod->path);
		g_slice_free(module, mod);
		return NULL;
	}

	/* temporary strings for mod_xyz_init and mod_xyz_free */
	m_init_str = g_string_new(name);
	g_string_append_len(m_init_str, CONST_STR_LEN("_init"));
	m_free_str = g_string_new(name);
	g_string_append_len(m_free_str, CONST_STR_LEN("_free"));

	if (!g_module_symbol(mod->module, m_init_str->str, (gpointer *)&m_init)
		|| !g_module_symbol(mod->module, m_free_str->str, (gpointer *)&mod->free)
		|| m_init == NULL || mod->free == NULL) {

		/* mod_init or mod_free couldn't be located, something went wrong */
		g_string_free(m_init_str, TRUE);
		g_string_free(m_free_str, TRUE);
		g_free(mod->path);
		g_string_free(mod->name, TRUE);
		g_slice_free(module, mod);
		return NULL;
	}

	/* call mod_xyz_init */
	if (!m_init(mods, mod)) {
		g_string_free(m_init_str, TRUE);
		g_string_free(m_free_str, TRUE);
		g_free(mod->path);
		g_string_free(mod->name, TRUE);
		g_slice_free(module, mod);
		return NULL;
	}

	/* insert into free slot */
	for (i = 0; i < mods->mods->len; i++) {
		if (!g_array_index(mods->mods, module*, i))
		{
			g_array_index(mods->mods, module*, i) = mod;
			break;
		}
	}

	/* if no free slot was found, append */
	if (i == mods->mods->len)
		g_array_append_val(mods->mods, mod);

	/* free temp strings */
	g_string_free(m_free_str, TRUE);
	g_string_free(m_init_str, TRUE);

	return mod;
}

void module_release(modules *mods, module *mod) {
	guint i;

	if (--mod->refcount > 0)
		return;

	for (i = 0; i < mods->mods->len; i++) {
		if (g_array_index(mods->mods, module*, i) == mod)
		{
			g_array_index(mods->mods, module*, i) = NULL;
			break;
		}
	}

	mod->free(mods, mod);
	g_module_close(mod->module);
	g_free(mod->path);
	g_string_free(mod->name, TRUE);
	g_slice_free(module, mod);
}

void module_release_name(modules *mods, const gchar* name) {
	module *mod = module_lookup(mods, name);

	if (mod)
		module_release(mods, mod);
}

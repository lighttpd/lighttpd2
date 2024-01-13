
#include <lighttpd/module.h>
#include <lighttpd/utils.h>

GQuark li_modules_error_quark(void) {
	return g_quark_from_string("li-modules-error-quark");
}

liModules *li_modules_new(gpointer main, const gchar *module_dir, gboolean module_resident) {
	liModules *m = g_slice_new(liModules);

	m->version = MODULE_VERSION;
	m->main = main;
	m->mods = g_array_new(FALSE, TRUE, sizeof(liModule*));
	m->module_dir = g_strdup(module_dir);
	m->module_resident = module_resident;
	m->sizeof_off_t = sizeof(off_t);

	return m;
}

liModule *li_module_lookup(liModules *mods, const gchar *name) {
	liModule *mod;
	GArray *a = mods->mods;
	guint i;

	for (i = 0; i < a->len; i++) {
		mod = g_array_index(a, liModule*, i);
		if (mod != NULL && g_str_equal(mod->name->str, name))
			return mod;
	}

	return NULL;
}

void li_modules_free(liModules* mods) {
	/* unload all modules */
	GArray *a = mods->mods;
	liModule *mod;
	guint i;

	for (i = 0; i < a->len; i++) {
		mod = g_array_index(a, liModule*, i);
		if (!mod)
			continue;
		li_module_release(mods, mod);
	}

	g_array_free(mods->mods, TRUE);
	g_free(mods->module_dir);
	g_slice_free(liModules, mods);
}


liModule* li_module_load(liModules *mods, const gchar* name, GError **err) {
	liModule *mod;
	liModuleInitCB m_init;
	GString *m_init_str, *m_free_str;
	guint i;

	mod = li_module_lookup(mods, name);

	if (mod) {
		/* module already loaded, increment refcount and return */
		mod->refcount++;
		return mod;
	}

	mod = g_slice_new0(liModule);
	mod->name = g_string_new(name);
	mod->refcount = 1;

	mod->path = g_module_build_path(mods->module_dir, name);
	mod->module = g_module_open(mod->path, G_MODULE_BIND_LAZY);

	if (!mod->module) {
		if (err) g_set_error(err, LI_MODULES_ERROR, 1, "%s", g_module_error());
		g_string_free(mod->name, TRUE);
		g_free(mod->path);
		g_slice_free(liModule, mod);
		return NULL;
	}

	/* temporary strings for mod_xyz_init and mod_xyz_free */
	m_init_str = g_string_new(name);
	li_g_string_append_len(m_init_str, CONST_STR_LEN("_init"));
	m_free_str = g_string_new(name);
	li_g_string_append_len(m_free_str, CONST_STR_LEN("_free"));

	if (!g_module_symbol(mod->module, m_init_str->str, (gpointer *)&m_init)
		|| !g_module_symbol(mod->module, m_free_str->str, (gpointer *)&mod->free)
		|| m_init == NULL || mod->free == NULL) {

		g_set_error(err, LI_MODULES_ERROR, 1,
			"li_module_load: couldn't load %s or %s from %s",
			m_init_str->str,
			m_free_str->str,
			mod->path);

		/* mod_init or mod_free couldn't be located, something went wrong */
		g_string_free(m_init_str, TRUE);
		g_string_free(m_free_str, TRUE);
		g_free(mod->path);
		g_string_free(mod->name, TRUE);
		g_slice_free(liModule, mod);
		return NULL;
	}

	/* call mod_xyz_init */
	if (!m_init(mods, mod)) {
		g_set_error(err, LI_MODULES_ERROR, 1,
			"li_module_load: calling %s from %s failed",
			m_init_str->str,
			mod->path);

		g_string_free(m_init_str, TRUE);
		g_string_free(m_free_str, TRUE);
		g_free(mod->path);
		g_string_free(mod->name, TRUE);
		g_slice_free(liModule, mod);
		return NULL;
	}

	if (mods->module_resident)
		g_module_make_resident(mod->module);

	/* insert into free slot */
	for (i = 0; i < mods->mods->len; i++) {
		if (!g_array_index(mods->mods, liModule*, i))
		{
			g_array_index(mods->mods, liModule*, i) = mod;
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

void li_module_release(liModules *mods, liModule *mod) {
	guint i;

	if (--mod->refcount > 0)
		return;

	for (i = 0; i < mods->mods->len; i++) {
		if (g_array_index(mods->mods, liModule*, i) == mod)
		{
			g_array_index(mods->mods, liModule*, i) = NULL;
			break;
		}
	}

	mod->free(mods, mod);
	g_module_close(mod->module);
	g_free(mod->path);
	g_string_free(mod->name, TRUE);
	g_slice_free(liModule, mod);
}

void li_module_release_name(liModules *mods, const gchar* name) {
	liModule *mod = li_module_lookup(mods, name);

	if (mod)
		li_module_release(mods, mod);
}

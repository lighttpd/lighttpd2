#ifndef _LIGHTTPD_MODULE_H_
#define _LIGHTTPD_MODULE_H_

#include "settings.h"

#define MODULE_VERSION ((guint) 0x00000001)
#define MODULE_VERSION_CHECK(mods) do { \
	if (mods->version != MODULE_VERSION) { \
		ERROR(mods->main, "Version mismatch for modules system: is %u, expected %u", mods->version, MODULE_VERSION); \
		return FALSE; \
	} } while(0)

/** see module_load */
#define MODULE_DEPENDS(mods, name) do { \
	if (!module_load(mods, name)) { \
		ERROR(mods->main, "Couldn't load dependency '%s'", name); \
		return FALSE; \
	} } while(0)

struct module;
typedef struct module module;

struct modules;
typedef struct modules modules;

/** Type of plugin_init function in modules */
typedef gboolean (*ModuleInit)(modules *mods, module *mod);
typedef gboolean (*ModuleFree)(modules *mods, module *mod);

struct module {
	gint refcount;    /**< count how often module is used. module gets unloaded if refcount reaches zero. */
	GString *name;      /**< name of module, can be set my plugin_init */
	GModule *module;  /**< glib handle */
	gchar *path;      /**< path to the module file */

	gpointer config;  /**< private module data */
	ModuleFree free;  /**< if set by plugin_init it gets called before module is unloaded */
};

struct modules {
	guint version;    /**< api version */

	gpointer main;    /**< pointer to a application specific main structure, e.g. server */
	GArray *mods;      /**< array of (module*) */
	gchar *module_dir;
};

LI_API modules* modules_init(gpointer main, const gchar *module_dir);
LI_API void modules_cleanup(modules *mods);

/** Loads a module if not loaded yet and returns the module struct for it (after increasing refcount)
  * returns NULL if it couldn't load the module.
  *
  * You should release modules after you used them with module_release or module_release_name */
LI_API module* module_load(modules *mods, const gchar* name);

LI_API void module_acquire(module *mod);
LI_API void module_release(modules *mods, module *mod);
LI_API void module_release_name(modules *mods, const gchar* name);

#endif

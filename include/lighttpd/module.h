#ifndef _LIGHTTPD_MODULE_H_
#define _LIGHTTPD_MODULE_H_

#include <lighttpd/settings.h>

#define MODULE_VERSION ((guint) 0x00000001)
#define MODULE_VERSION_CHECK(mods) do { \
		if (mods->version != MODULE_VERSION) { \
			ERROR(mods->main, "Version mismatch for modules system: is %u, expected %u", mods->version, MODULE_VERSION); \
			return FALSE; \
		} \
		if (mods->sizeof_off_t != (guint8)sizeof(off_t)) { \
			ERROR(mods->main, "Compile flags mismatch: sizeof(off_t) is %u, expected %u", (guint) sizeof(off_t), mods->sizeof_off_t); \
			return FALSE; \
		} \
	} while(0)

/** see li_module_load */
#define MODULE_DEPENDS(mods, name) do { \
	GError *err = NULL; \
	if (!li_module_load(mods, name, &err)) { \
		ERROR(mods->main, "Couldn't load dependency '%s': %s", name, err->message); \
		g_error_free(err); \
		return FALSE; \
	} } while(0)

#define LI_MODULES_ERROR li_modules_error_quark()
LI_API GQuark li_modules_error_quark(void);

typedef struct liModule liModule;

typedef struct liModules liModules;

/** Type of plugin_init function in modules */
typedef gboolean (*liModuleInitCB)(liModules *mods, liModule *mod);
typedef gboolean (*liModuleFreeCB)(liModules *mods, liModule *mod);

struct liModule {
	gint refcount;    /**< count how often module is used. module gets unloaded if refcount reaches zero. */
	GString *name;      /**< name of module, can be set my plugin_init */
	GModule *module;  /**< glib handle */
	gchar *path;      /**< path to the module file */

	gpointer config;  /**< private module data */
	liModuleFreeCB free;  /**< if set by plugin_init it gets called before module is unloaded */
};

struct liModules {
	guint version;    /**< api version */

	gpointer main;    /**< pointer to a application specific main structure, e.g. server */
	GArray *mods;      /**< array of (module*) */
	gchar *module_dir;
	gboolean module_resident; /** if true, call g_module_make_resident() when loading a module */

	guint8 sizeof_off_t; /** holds the value of sizeof(off_t) to check if loaded module was compiled with the same flags */
};

LI_API liModules* li_modules_new(gpointer main, const gchar *module_dir, gboolean make_resident);
LI_API void li_modules_free(liModules *mods);

/** Loads a module if not loaded yet and returns the module struct for it (after increasing refcount)
  * returns NULL if it couldn't load the module.
  *
  * You should release modules after you used them with li_module_release or li_module_release_name */
LI_API liModule* li_module_load(liModules *mods, const gchar* name, GError **error);

/* find module by name */
LI_API liModule *li_module_lookup(liModules *mods, const gchar *name);

LI_API void li_module_acquire(liModule *mod);
LI_API void li_module_release(liModules *mods, liModule *mod);
LI_API void li_module_release_name(liModules *mods, const gchar* name);

#endif

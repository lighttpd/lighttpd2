#ifndef _LIGHTTPD_ANGEL_PLUGIN_H_
#define _LIGHTTPD_ANGEL_PLUGIN_H_

#ifndef _LIGHTTPD_ANGEL_BASE_H_
#error Please include <lighttpd/angel_base.h> instead of this file
#endif

typedef struct liPluginItem liPluginItem;
typedef struct liPlugin liPlugin;
typedef struct liPlugins liPlugins;

typedef gboolean (*liPluginInitCB)                (liServer *srv, liPlugin *p);
typedef void     (*liPluginFreeCB)                (liServer *srv, liPlugin *p);

typedef void     (*liPluginCleanConfigCB)         (liServer *srv, liPlugin *p);
typedef gboolean (*liPluginCheckConfigCB)         (liServer *srv, liPlugin *p, GError **err);
typedef void     (*liPluginActivateConfigCB)      (liServer *srv, liPlugin *p);
typedef gboolean (*liPluginParseItemCB)           (liServer *srv, liPlugin *p, liValue *value, GError **err);

typedef void     (*liPluginHandleCallCB)          (liServer *srv, liPlugin *p, liInstance *i, gint32 id, GString *data);

typedef void     (*liPluginInstanceReplacedCB)    (liServer *srv, liPlugin *p, liInstance *oldi, liInstance *newi);
typedef void     (*liPluginInstanceReachedStateCB)(liServer *srv, liPlugin *p, liInstance *i, liInstanceState s);

struct liPluginItem {
	const gchar *name;
	liPluginParseItemCB handle_parse_item;
};

struct liPlugin {
	size_t version;
	const gchar *name; /**< name of the plugin */

	gpointer data;     /**< private plugin data */

	const liPluginItem *items;
	GHashTable *angel_callbacks; /**< map (const gchar*) -> liPluginHandleCallCB */

	liPluginFreeCB handle_free;   /**< called before plugin is unloaded */

	liPluginCleanConfigCB handle_clean_config;        /**< called before the reloading of the config is started or after the reloading failed */
	liPluginCheckConfigCB handle_check_config;        /**< called before activating a config to ensure everything works */
	liPluginActivateConfigCB handle_activate_config;  /**< called to activate a config after successful loading it. this cannot fail */

	liPluginInstanceReplacedCB handle_instance_replaced;
	liPluginInstanceReachedStateCB handle_instance_reached_state;
};

struct liPlugins {
	GString *config_filename;

	GHashTable *items, *load_items; /**< gchar* -> server_item */

	liModules *modules;

	GHashTable *module_refs, *load_module_refs; /** gchar* -> server_module */
	GHashTable *ht_plugins, *load_ht_plugins;

	GPtrArray *plugins, *load_plugins; /* plugin* */
};

LI_API void li_plugins_init(liServer *srv, const gchar *module_dir, gboolean module_resident);
LI_API void li_plugins_clear(liServer *srv);

LI_API void li_plugins_config_clean(liServer *srv);
LI_API gboolean li_plugins_config_load(liServer *srv, const gchar *filename);

LI_API gboolean li_plugins_handle_item(liServer *srv, GString *itemname, liValue *parameters, GError **err);

/* "core" is a reserved module name for interal use */
LI_API gboolean li_plugins_load_module(liServer *srv, const gchar *name);
/* Needed by modules to register their plugin(s) */
LI_API liPlugin *li_angel_plugin_register(liServer *srv, liModule *mod, const gchar *name, liPluginInitCB init);
INLINE void li_angel_plugin_add_angel_cb(liPlugin *p, const gchar *name, liPluginHandleCallCB cb);

/* called when replace was successful or failed - check states to find out */
LI_API void li_angel_plugin_replaced_instance(liServer *srv, liInstance *oldi, liInstance *newi);
LI_API void li_angel_plugin_instance_reached_state(liServer *srv, liInstance *i, liInstanceState s);

/* inline implementations */

INLINE void li_angel_plugin_add_angel_cb(liPlugin *p, const gchar *name, liPluginHandleCallCB cb) {
	g_hash_table_insert(p->angel_callbacks, (gchar*) name, (gpointer)(intptr_t) cb);
}

#endif

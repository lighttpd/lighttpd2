#ifndef _LIGHTTPD_ANGEL_PLUGIN_H_
#define _LIGHTTPD_ANGEL_PLUGIN_H_

#ifndef _LIGHTTPD_ANGEL_BASE_H_
#error Please include <lighttpd/angel_base.h> instead of this file
#endif

typedef struct liPluginItem liPluginItem;
typedef struct liPluginItemOption liPluginItemOption;
typedef struct liPlugin liPlugin;
typedef struct liPlugins liPlugins;

typedef gboolean (*liPluginInitCB)          (liServer *srv, liPlugin *p);
typedef void     (*liPluginFreeCB)          (liServer *srv, liPlugin *p);

typedef void     (*liPluginCleanConfigCB)   (liServer *srv, liPlugin *p);
typedef gboolean (*liPluginCheckConfigCB)   (liServer *srv, liPlugin *p);
typedef void     (*liPluginActivateConfigCB)(liServer *srv, liPlugin *p);
typedef void     (*liPluginParseItemCB)     (liServer *srv, liPlugin *p, liValue **options);

typedef void     (*liPluginHandleCallCB)    (liServer *srv, liInstance *i, liPlugin *p, gint32 id, GString *data);

typedef enum {
	LI_PLUGIN_ITEM_OPTION_MANDATORY = 1
} liPluginItemOptionFlags;

struct liPluginItemOption {
	const gchar *name; /**< name of the option */
	liValueType type;   /**< type of the option; may be LI_VALUE_NONE to accept anything */
	liPluginItemOptionFlags flags; /**< flags of the option */
};

struct liPluginItem {
	const gchar *name;
	liPluginParseItemCB handle_parse_item;

	const liPluginItemOption *options;
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
};

struct liPlugins {
	GString *config_filename;

	GHashTable *items, *load_items; /**< gchar* -> server_item */

	liModules *modules;

	GHashTable *module_refs, *load_module_refs; /** gchar* -> server_module */
	GHashTable *ht_plugins, *load_ht_plugins;

	GPtrArray *plugins, *load_plugins; /* plugin* */
};

LI_API void li_plugins_init(liServer *srv, const gchar *module_dir);
LI_API void li_plugins_clear(liServer *srv);

LI_API void li_plugins_config_clean(liServer *srv);
LI_API gboolean li_plugins_config_load(liServer *srv, const gchar *filename);

LI_API void li_plugins_handle_item(liServer *srv, GString *itemname, liValue *hash);

/* "core" is a reserved module name for interal use */
LI_API gboolean li_plugins_load_module(liServer *srv, const gchar *name);
/* Needed by modules to register their plugin(s) */
LI_API liPlugin *li_angel_plugin_register(liServer *srv, liModule *mod, const gchar *name, liPluginInitCB init);

#endif

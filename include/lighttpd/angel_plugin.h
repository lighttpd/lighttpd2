#ifndef _LIGHTTPD_ANGEL_PLUGIN_H_
#define _LIGHTTPD_ANGEL_PLUGIN_H_

#ifndef _LIGHTTPD_ANGEL_BASE_H_
#error Please include <lighttpd/angel_base.h> instead of this file
#endif

typedef struct plugin_item plugin_item;
typedef struct plugin_item_option plugin_item_option;
typedef struct plugin plugin;
typedef struct Plugins Plugins;

typedef gboolean (*PluginInit)          (server *srv, plugin *p);
typedef void     (*PluginFree)          (server *srv, plugin *p);

typedef void     (*PluginCleanConfig)   (server *srv, plugin *p);
typedef gboolean (*PluginCheckConfig)   (server *srv, plugin *p);
typedef void     (*PluginActivateConfig)(server *srv, plugin *p);
typedef void     (*PluginParseItem)     (server *srv, plugin *p, value **options);

typedef void     (*PluginHandleCall)    (server *srv, instance *i, plugin *p, gint32 id, GString *data);

typedef enum {
	PLUGIN_ITEM_OPTION_MANDATORY = 1
} plugin_item_option_flags;

struct plugin_item_option {
	const gchar *name; /**< name of the option */
	value_type type;   /**< type of the option; may be VALUE_NONE to accept anything */
	plugin_item_option_flags flags; /**< flags of the option */
};

struct plugin_item {
	const gchar *name;
	PluginParseItem handle_parse_item;

	const plugin_item_option *options;
};

struct plugin {
	size_t version;
	const gchar *name; /**< name of the plugin */

	gpointer data;     /**< private plugin data */

	const plugin_item *items;
	GHashTable *angel_callbacks; /**< map (const gchar*) -> PluginHandleCall */

	PluginFree handle_free;   /**< called before plugin is unloaded */

	PluginCleanConfig handle_clean_config;        /**< called before the reloading of the config is started or after the reloading failed */
	PluginCheckConfig handle_check_config;        /**< called before activating a config to ensure everything works */
	PluginActivateConfig handle_activate_config;  /**< called to activate a config after successful loading it. this cannot fail */
};

struct Plugins {
	GString *config_filename;

	GHashTable *items, *load_items; /**< gchar* -> server_item */

	struct modules *modules;

	GHashTable *module_refs, *load_module_refs; /** gchar* -> server_module */
	GHashTable *ht_plugins, *load_ht_plugins;

	GPtrArray *plugins, *load_plugins; /* plugin* */
};

void plugins_init(server *srv, const gchar *module_dir);
void plugins_clear(server *srv);

void plugins_config_clean(server *srv);
gboolean plugins_config_load(server *srv, const gchar *filename);

void plugins_handle_item(server *srv, GString *itemname, value *hash);

/* "core" is a reserved module name for interal use */
gboolean plugins_load_module(server *srv, const gchar *name);
/* Needed by modules to register their plugin(s) */
LI_API plugin *angel_plugin_register(server *srv, module *mod, const gchar *name, PluginInit init);

#endif

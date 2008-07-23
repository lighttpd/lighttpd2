#ifndef _LIGHTTPD_PLUGIN_H_
#define _LIGHTTPD_PLUGIN_H_

struct plugin;
typedef struct plugin plugin;

struct plugin_option;
typedef struct plugin_option plugin_option;

struct server_option;
typedef struct server_option server_option;

struct plugin_action;
typedef struct plugin_action plugin_action;

struct server_action;
typedef struct server_action server_action;

struct plugin_setup;
typedef struct plugin_setup plugin_setup;

struct server_setup;
typedef struct server_setup server_setup;

#define INIT_FUNC(x) \
		LI_EXPORT void * x(server *srv, plugin *)

#define PLUGIN_DATA \
	size_t id; \
	ssize_t option_base_ndx

#include "base.h"
#include "options.h"
#include "actions.h"
#include "module.h"

typedef void     (*PluginInit)          (server *srv, plugin *p);
typedef void     (*PluginFree)          (server *srv, plugin *p);
typedef gboolean (*PluginParseOption)   (server *srv, gpointer p_d, size_t ndx, option *opt, gpointer *value);
typedef void     (*PluginFreeOption)    (server *srv, gpointer p_d, size_t ndx, gpointer value);
typedef gboolean (*PluginCreateAction)  (server *srv, gpointer p_d, option *opt, action_func *func);
typedef gboolean (*PluginSetup)         (server *srv, gpointer p_d, option *opt);

struct plugin {
	size_t version;
	const gchar *name; /**< name of the plugin */

	gpointer data;    /**< private plugin data */

	PluginFree free; /**< called before plugin is unloaded */

	const plugin_option *options;
	const plugin_action *actions;
	const plugin_setup *setups;
};

struct plugin_option {
	const gchar *name;
	option_type type;

	PluginParseOption parse_option;
	PluginFreeOption free_option;
};

struct plugin_action {
	const gchar *name;
	PluginCreateAction create_action;
};

struct plugin_setup {
	const gchar *name;
	PluginSetup setup;
};

/* Internal structures */
struct server_option {
	plugin *p;

	/** the plugin must free the _content_ of the option
	  * opt is zero to get the global default value if nothing is specified
	  * save result in value
	  *
	  * Default behaviour (NULL) is to just use the option as value
	  */
	PluginParseOption parse_option;
	PluginFreeOption free_option;

	size_t index, module_index;
	option_type type;
};

struct server_action {
	plugin *p;
	PluginCreateAction create_action;
};

struct server_setup {
	plugin *p;
	PluginSetup setup;
};

LI_API void plugin_free(server *srv, plugin *p);
LI_API gboolean plugin_register(server *srv, const gchar *name, PluginInit init);

LI_API gboolean parse_option(server *srv, const char *key, option *opt, option_set *mark);
LI_API void release_option(server *srv, option_set *mark); /**< Does not free the option_set memory */

#endif

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
typedef gboolean (*PluginParseOption)   (server *srv, plugin *p, size_t ndx, option *opt, gpointer *value);
typedef void     (*PluginFreeOption)    (server *srv, plugin *p, size_t ndx, gpointer value);
typedef gpointer (*PluginDefaultValue) (server *srv, plugin *p, gsize ndx);
typedef action*  (*PluginCreateAction)  (server *srv, plugin *p, option *opt);
typedef gboolean (*PluginSetup)         (server *srv, plugin *p, option *opt);

typedef void     (*PluginHandleContent) (server *srv, connection *con, plugin *p);
typedef void     (*PluginHandleClose)   (server *srv, connection *con, plugin *p);

struct plugin {
	size_t version;
	const gchar *name; /**< name of the plugin */

	gpointer data;     /**< private plugin data */

	size_t opt_base_index;

	PluginFree free;   /**< called before plugin is unloaded */

	/** called if plugin registered as indirect handler with connection_handle_indirect(srv, con, p)
	  *  - after response headers are created:
	  *      connection_set_state(srv, con, CON_STATE_HANDLE_RESPONSE_HEADER)
	  *  - after content is generated close output queue:
	  *      con->out->is_closed = TRUE
	  */
	PluginHandleContent handle_content;

	/** called for every plugin after connection got closed (response end, reset by peer, error)
	  * the plugins code must not depend on any order of plugins loaded
	  */
	PluginHandleClose handle_close;

	const plugin_option *options;
	const plugin_action *actions;
	const plugin_setup *setups;
};

struct plugin_option {
	const gchar *name;
	option_type type;

	PluginDefaultValue default_value;
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
	PluginDefaultValue default_value; /* default value callback - if no callback is provided, default value will be NULL, 0 or FALSE */
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

/* Needed my modules to register their plugin(s) */
LI_API gboolean plugin_register(server *srv, const gchar *name, PluginInit init);

/* Internal needed functions */
LI_API void plugin_free(server *srv, plugin *p);

LI_API gboolean parse_option(server *srv, const char *name, option *opt, option_set *mark);
LI_API void release_option(server *srv, option_set *mark); /**< Does not free the option_set memory */

LI_API void plugins_prepare_callbacks(server *srv);
LI_API void plugins_handle_close(server *srv, connection *con);

/* Needed for config frontends */
/** For parsing 'somemod.option = "somevalue"' */
LI_API action* option_action(server *srv, const gchar *name, option *value);
/** For parsing 'somemod.action value', e.g. 'rewrite "/url" => "/destination"'
  * You need to free the option after it (it should be of type NONE then)
  */
LI_API action* create_action(server *srv, const gchar *name, option *value);
/** For setup function, e.g. 'listen "127.0.0.1:8080"' */
LI_API gboolean call_setup(server *srv, const char *name, option *opt);

/* needs connection *con and plugin *p */
#define OPTION(idx) _OPTION(con, p, idx)
#define _OPTION(con, p, idx) (con->options[p->opt_base_index + idx])
#define _OPTION_ABS(con, idx) (con->options[idx])

#endif

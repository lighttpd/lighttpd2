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
typedef gboolean (*PluginParseOption)   (server *srv, plugin *p, size_t ndx, value *val, option_value *oval);
typedef void     (*PluginFreeOption)    (server *srv, plugin *p, size_t ndx, option_value oval);
typedef action*  (*PluginCreateAction)  (server *srv, plugin *p, value *val);
typedef gboolean (*PluginSetup)         (server *srv, plugin *p, value *val);

typedef void     (*PluginHandleContent) (connection *con, plugin *p);
typedef void     (*PluginHandleClose)   (connection *con, plugin *p);

struct plugin {
	size_t version;
	const gchar *name; /**< name of the plugin */

	gpointer data;     /**< private plugin data */

	size_t opt_base_index;

	PluginFree free;   /**< called before plugin is unloaded */

	/** called if plugin registered as indirect handler with connection_handle_indirect(srv, con, p)
	  *  - after response headers are created:
	  *      connection_set_state(con, CON_STATE_HANDLE_RESPONSE_HEADER)
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
	value_type type;

	gpointer default_value;
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

	/** the value is freed with value_free after the parse call, so you
	  *   probably want to extract the content via value_extract*
	  * val is zero to get the global default value if nothing is specified
	  * save result in value
	  *
	  * Default behaviour (NULL) is to extract the inner value from val
	  */
	PluginParseOption parse_option;

	/** the free_option handler has to free all allocated resources;
	  * it may get called with 0 initialized options, so you have to
	  * check the value.
	  */
	PluginFreeOption free_option;

	/** if parse_option is NULL, the default_value is used; it is only used
	  * for the following value types:
	  * - BOOLEAN, NUMBER: casted with GPOINTER_TO_INT, i.e. set it with GINT_TO_POINTER
	  *     the numbers are limited to the 32-bit range according to the glib docs
	  * - STRING: used for g_string_new, i.e. a const char*
	  */
	gpointer default_value;

	size_t index, module_index;
	value_type type;
};

struct server_action {
	plugin *p;
	PluginCreateAction create_action;
};

struct server_setup {
	plugin *p;
	PluginSetup setup;
};

/* Needed by modules to register their plugin(s) */
LI_API gboolean plugin_register(server *srv, const gchar *name, PluginInit init);

/* Internal needed functions */
LI_API void plugin_free(server *srv, plugin *p);
LI_API void server_plugins_free(server *srv);

/** free val after call (val may be modified by parser) */
LI_API gboolean parse_option(server *srv, const char *name, value *val, option_set *mark);
LI_API void release_option(server *srv, option_set *mark); /**< Does not free the option_set memory */

LI_API void plugins_prepare_callbacks(server *srv);
LI_API void plugins_handle_close(connection *con);

/* Needed for config frontends */
/** For parsing 'somemod.option = "somevalue"', free value after call */
LI_API action* option_action(server *srv, const gchar *name, value *value);
/** For parsing 'somemod.action value', e.g. 'rewrite "/url" => "/destination"'
  * free value after call
  */
LI_API action* create_action(server *srv, const gchar *name, value *value);
/** For setup function, e.g. 'listen "127.0.0.1:8080"'; free value after call */
LI_API gboolean call_setup(server *srv, const char *name, value *val);

LI_API void plugins_free_default_options(server *srv);


/** free val after call */
LI_API gboolean plugin_set_default_option(server *srv, const gchar* name, value *val);

/* needs connection *con and plugin *p */
#define OPTION(idx) _OPTION(con, p, idx)
#define _OPTION(con, p, idx) (con->options[p->opt_base_index + idx])
#define _OPTION_ABS(con, idx) (con->options[idx])

#endif

#ifndef _LIGHTTPD_PLUGIN_H_
#define _LIGHTTPD_PLUGIN_H_

struct plugin;
typedef struct plugin plugin;

struct module_option;
typedef struct module_option module_option;

struct server_option;
typedef struct server_option server_option;

#define INIT_FUNC(x) \
		LI_EXPORT void * x(server *srv, plugin *)

#define PLUGIN_DATA \
	size_t id; \
	ssize_t option_base_ndx

#include "base.h"
#include "options.h"

typedef void     (*ModuleInit)        (server *srv, plugin *p);
typedef void     (*ModuleFree)        (server *srv, plugin *p);
typedef gboolean (*ModuleParseOption) (server *srv, gpointer p_d, size_t ndx, option *opt, gpointer *value);
typedef void     (*ModuleFreeOption)  (server *srv, gpointer p_d, size_t ndx, gpointer value);

struct module {
	GString *name;

	GModule *lib;
};


struct plugin {
	size_t version;

	GString *name; /* name of the plugin */

	gpointer data;

	ModuleFree *free;

	module_option *options;
};

struct module_option {
	const char *key;
	option_type type;

	ModuleParseOption parse_option;
	ModuleFreeOption free_option;
};

struct server_option {
	plugin *p;

	/* the plugin must free the _content_ of the option
	 * opt is zero to get the global default value if nothing is specified
	 * save result in value
	 *
	 * Default behaviour (NULL) is to just use the option as value
	 */
	ModuleParseOption parse_option;
	ModuleFreeOption free_option;

	size_t index, module_index;
	option_type type;
};

LI_API gboolean plugin_register(server *srv, ModuleInit *init);

LI_API gboolean parse_option(server *srv, const char *key, option *opt, option_set *mark);
LI_API void release_option(server *srv, option_set *mark); /** Does not free the option_set memory */

LI_API gboolean plugin_load(server *srv, const char *module);

#endif

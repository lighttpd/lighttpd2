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

struct plugin {
	size_t version;

	GString *name; /* name of the plugin */

	void *(* init)              (server *srv, plugin *p);

	gpointer data;

	/* dlopen handle */
	void *lib;

	module_option *options;
};

struct module_option {
	const char *key;
	option_type type;
};

struct server_option {
	plugin *p;

	/* the plugin must free the _content_ of the option
	 * opt is zero to get the global default value if nothing is specified
	 * save result in value
	 */
	gboolean (* parse_option)        (server *srv, void *p_d, size_t ndx, option *opt, gpointer *value);
	void (* free_option)             (server *srv, void *p_d, size_t ndx, gpointer value);

	size_t index, module_index;
	option_type type;
};

LI_API gboolean parse_option(server *srv, const char *key, option *opt, option_mark *mark);

#endif

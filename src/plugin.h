#ifndef _PLUGIN_H_
#define _PLUGIN_H_

#include "base.h"
#include "option.h"

struct plugin;
typedef struct plugin plugin;

struct module_option;
typedef struct module_option module_option;

#define INIT_FUNC(x) \
		LI_EXPORT void * x(server *srv, plugin *)

#define PLUGIN_DATA \
	size_t id; \
	ssize_t option_base_ndx

struct plugin {
	size_t version;

	GString *name; /* name of the plugin */

	void *(* init)              (server *srv, plugin *p);
	/* the plugin must free the _content_ of the option */
	option_mark *(* parse_option)    (server *src, void *p_d, size_t option_ndx, option *option);

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
	size_t index, module_index;
	option_type type;
};

#endif

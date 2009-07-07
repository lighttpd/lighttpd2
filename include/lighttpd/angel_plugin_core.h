#ifndef _LIGHTTPD_ANGEL_PLUGIN_CORE_H_
#define _LIGHTTPD_ANGEL_PLUGIN_CORE_H_

#include <lighttpd/angel_base.h>

typedef struct {
	/* Load */
	instance_conf *load_instconf;
	gboolean load_failed;

	/* Running */
	instance_conf *instconf;
	instance *inst;
} plugin_core_config_t;

gboolean plugin_core_init(server *srv);

#endif

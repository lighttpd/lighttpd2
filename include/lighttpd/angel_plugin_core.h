#ifndef _LIGHTTPD_ANGEL_PLUGIN_CORE_H_
#define _LIGHTTPD_ANGEL_PLUGIN_CORE_H_

#include <lighttpd/angel_base.h>

typedef struct liPluginCoreConfig liPluginCoreConfig;
struct liPluginCoreConfig {
	/* Load */
	liInstanceConf *load_instconf;
	gboolean load_failed;

	/* Running */
	liInstanceConf *instconf;
	liInstance *inst;
};

gboolean plugin_core_init(liServer *srv);

#endif

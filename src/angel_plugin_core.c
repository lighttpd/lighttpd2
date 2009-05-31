
#include <lighttpd/angel_plugin_core.h>

gboolean plugin_core_init(server *srv) {
	/* load core plugins */
	UNUSED(srv);
	return TRUE;
}
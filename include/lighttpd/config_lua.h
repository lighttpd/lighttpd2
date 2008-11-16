#ifndef _LIGHTTPD_CONFIG_LUA_H_
#define _LIGHTTPD_CONFIG_LUA_H_

#include <lighttpd/base.h>

LI_API gboolean config_lua_load(server *srv, const gchar *filename);

#endif

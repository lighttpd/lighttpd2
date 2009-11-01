#ifndef _LIGHTTPD_CONFIG_LUA_H_
#define _LIGHTTPD_CONFIG_LUA_H_

#include <lighttpd/base.h>

#include <lualib.h>

LI_API gboolean li_config_lua_load(lua_State *L, liServer *srv, const gchar *filename, liAction **pact, gboolean allow_setup);

#endif

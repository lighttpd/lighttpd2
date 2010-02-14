#ifndef _LIGHTTPD_CONFIG_LUA_H_
#define _LIGHTTPD_CONFIG_LUA_H_

#include <lighttpd/base.h>

#include <lualib.h>

LI_API gboolean li_config_lua_load(lua_State *L, liServer *srv, const gchar *filename, liAction **pact, gboolean allow_setup, liValue *args);

LI_API gboolean li_lua_config_publish_str_hash(liServer *srv, lua_State *L, GHashTable *ht, int (*wrapper)(liServer *srv, lua_State *L, gpointer data));
LI_API int li_lua_config_handle_server_action(liServer *srv, lua_State *L, gpointer _sa);
LI_API int li_lua_config_handle_server_setup(liServer *srv, lua_State *L, gpointer _ss);

#endif

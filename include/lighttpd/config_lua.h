#ifndef _LIGHTTPD_CONFIG_LUA_H_
#define _LIGHTTPD_CONFIG_LUA_H_

#include <lighttpd/base.h>

#include <lualib.h>

LI_API gboolean li_config_lua_load(liLuaState *LL, liServer *srv, liWorker *wrk, const gchar *filename, liAction **pact, gboolean allow_setup, liValue *args);

LI_API void li_lua_push_action_table(liServer *srv, liWorker *wrk, lua_State *L);
LI_API void li_lua_push_setup_table(liServer *srv, liWorker *wrk, lua_State *L);

#endif

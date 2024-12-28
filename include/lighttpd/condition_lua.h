#ifndef _LIGHTTPD_CONDITION_LUA_H_
#define _LIGHTTPD_CONDITION_LUA_H_

#include <lighttpd/base.h>
#include <lua.h>

LI_API void li_lua_init_condition_mt(liServer *srv, lua_State *L);

LI_API liCondition* li_lua_get_condition(lua_State *L, int ndx);
LI_API int li_lua_push_condition(liServer *srv, lua_State *L, liCondition *c);

LI_API void li_lua_set_global_condition_lvalues(liServer *srv, lua_State *L);

#endif

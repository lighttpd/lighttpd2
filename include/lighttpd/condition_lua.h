#ifndef _LIGHTTPD_CONDITION_LUA_H_
#define _LIGHTTPD_CONDITION_LUA_H_

#include <lighttpd/base.h>
#include <lua.h>

LI_API liCondition* lua_get_condition(lua_State *L, int ndx);
LI_API int lua_push_condition(liServer *srv, lua_State *L, liCondition *c);

LI_API void lua_push_lvalues_dict(liServer *srv, lua_State *L);

#endif

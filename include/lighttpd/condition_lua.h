#ifndef _LIGHTTPD_CONDITION_LUA_H_
#define _LIGHTTPD_CONDITION_LUA_H_

#include <lighttpd/base.h>
#include <lua.h>

liCondition* lua_get_condition(lua_State *L, int ndx);
int lua_push_condition(liServer *srv, lua_State *L, liCondition *c);

void lua_push_lvalues_dict(liServer *srv, lua_State *L);

#endif

#ifndef _LIGHTTPD_CONDITION_LUA_H_
#define _LIGHTTPD_CONDITION_LUA_H_

#include <lighttpd/base.h>
#include <lua.h>

condition* lua_get_condition(lua_State *L, int ndx);
int lua_push_condition(server *srv, lua_State *L, condition *c);

void lua_push_lvalues_dict(server *srv, lua_State *L);

#endif

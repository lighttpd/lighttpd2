#ifndef _LIGHTTPD_ACTIONS_LUA_H_
#define _LIGHTTPD_ACTIONS_LUA_H_

#include <lighttpd/base.h>
#include <lua.h>

liAction* lua_get_action(lua_State *L, int ndx);
int lua_push_action(liServer *srv, lua_State *L, liAction *a);

#endif

#ifndef _LIGHTTPD_ACTIONS_LUA_H_
#define _LIGHTTPD_ACTIONS_LUA_H_

#include <lighttpd/base.h>
#include <lua.h>

LI_API liAction* lua_get_action(lua_State *L, int ndx);
LI_API int lua_push_action(liServer *srv, lua_State *L, liAction *a);

/* create new action from lua function */
LI_API liAction* lua_make_action(lua_State *L, int ndx);

#endif

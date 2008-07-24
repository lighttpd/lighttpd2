#ifndef _LIGHTTPD_ACTIONS_LUA_H_
#define _LIGHTTPD_ACTIONS_LUA_H_

#include "actions.h"
#include <lua.h>

action* lua_get_action(lua_State *L, int ndx);
int lua_push_action(server *srv, lua_State *L, action *a);
action_list* lua_get_actionlist(lua_State *L);

#endif


#include <lighttpd/actions_lua.h>

#include <lualib.h>
#include <lauxlib.h>

#define LUA_ACTION "action*"

action* lua_get_action(lua_State *L, int ndx) {
	if (!lua_isuserdata(L, ndx)) return NULL;
	if (!lua_getmetatable(L, ndx)) return NULL;
	luaL_getmetatable(L, LUA_ACTION);
	if (lua_isnil(L, -1) || lua_isnil(L, -2) || !lua_equal(L, -1, -2)) {
		lua_pop(L, 2);
		return NULL;
	}
	lua_pop(L, 2);
	return *(action**) lua_touserdata(L, ndx);
}

static int lua_action_gc(lua_State *L) {
	server *srv;
	action **a = (action**) luaL_checkudata(L, 1, LUA_ACTION);
	if (!a || !*a) return 0;

	srv = (server*) lua_touserdata(L, lua_upvalueindex(1));
	action_release(srv, *a);
	return 0;
}

int lua_push_action(server *srv, lua_State *L, action *a) {
	action **pa;

	pa = (action**) lua_newuserdata(L, sizeof(action*));
	*pa = a;

	if (luaL_newmetatable(L, LUA_ACTION)) {
		lua_pushlightuserdata(L, srv);
		lua_pushcclosure(L, lua_action_gc, 1);
		lua_setfield(L, -2, "__gc");
	}

	lua_setmetatable(L, -2);
	return 1;
}

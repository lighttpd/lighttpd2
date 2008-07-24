
#include "condition_lua.h"

#include <lualib.h>
#include <lauxlib.h>

#define LUA_CONDITION "condition*"

condition* lua_get_condition(lua_State *L, int ndx) {
	if (!lua_isuserdata(L, ndx)) return NULL;
	if (!lua_getmetatable(L, ndx)) return NULL;
	luaL_getmetatable(L, LUA_CONDITION);
	if (lua_isnil(L, -1) || lua_isnil(L, -2) || !lua_equal(L, -1, -2)) {
		lua_pop(L, 2);
		return NULL;
	}
	lua_pop(L, 2);
	return *(condition**) lua_touserdata(L, ndx);
}

static int lua_condition_gc(lua_State *L) {
	server *srv;
	condition **c = (condition**) luaL_checkudata(L, 1, LUA_CONDITION);
	if (!c || !*c) return 0;

	srv = (server*) lua_touserdata(L, lua_upvalueindex(1));
	condition_release(srv, *c);
	return 0;
}

int lua_push_condition(server *srv, lua_State *L, condition *c) {
	condition **pc;

	pc = (condition**) lua_newuserdata(L, sizeof(condition*));
	condition_acquire(c);
	*pc = c;

	if (luaL_newmetatable(L, LUA_CONDITION)) {
		lua_pushlightuserdata(L, srv);
		lua_pushcclosure(L, lua_condition_gc, 1);
		lua_setfield(L, -2, "__gc");
	}

	lua_setmetatable(L, -2);
	return 1;
}

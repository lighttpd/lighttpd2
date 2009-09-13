
#include <lighttpd/core_lua.h>

#include <lualib.h>
#include <lauxlib.h>

#define LUA_CONNECTION "liConnection*"

static void init_con_mt(lua_State *L) {
	/* TODO */
}

void lua_init_connection_mt(lua_State *L) {
	if (luaL_newmetatable(L, LUA_CONNECTION)) {
		init_con_mt(L);
	}
	lua_pop(L, 1);
}

liConnection* lua_get_connection(lua_State *L, int ndx) {
	if (!lua_isuserdata(L, ndx)) return NULL;
	if (!lua_getmetatable(L, ndx)) return NULL;
	luaL_getmetatable(L, LUA_CONNECTION);
	if (lua_isnil(L, -1) || lua_isnil(L, -2) || !lua_equal(L, -1, -2)) {
		lua_pop(L, 2);
		return NULL;
	}
	lua_pop(L, 2);
	return *(liConnection**) lua_touserdata(L, ndx);
}

int lua_push_connection(lua_State *L, liConnection *con) {
	liConnection **pcon;

	pcon = (liConnection**) lua_newuserdata(L, sizeof(liConnection*));
	*pcon = con;

	if (luaL_newmetatable(L, LUA_CONNECTION)) {
		init_con_mt(L);
	}

	lua_setmetatable(L, -2);
	return 1;
}

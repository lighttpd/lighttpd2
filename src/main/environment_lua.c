
#include <lighttpd/core_lua.h>

#include <lualib.h>
#include <lauxlib.h>

#define LUA_ENVIRONMENT "liEnvironment*"

static int lua_environment_get(lua_State *L) {
	liEnvironment *env;
	const char *ckey;
	size_t keylen;
	GString *val;

	luaL_checkany(L, 2);
	if (NULL == (env = li_lua_get_environment(L, 1))) return 0;
	if (NULL == (ckey = lua_tolstring(L, 2, &keylen))) return 0;

	val = li_environment_get(env, ckey, keylen);
	if (val) {
		lua_pushlstring(L, val->str, val->len);
	} else {
		lua_pushnil(L);
	}

	return 1;
}

static int lua_environment_index(lua_State *L) {
	if (li_lua_metatable_index(L)) return 1;

	return lua_environment_get(L);
}

static int lua_environment_set(lua_State *L) {
	liEnvironment *env;
	const char *ckey, *cval;
	size_t keylen, vallen;

	luaL_checkany(L, 3);
	if (NULL == (env = li_lua_get_environment(L, 1))) return 0;
	if (NULL == (ckey = lua_tolstring(L, 2, &keylen))) return 0;
	if (lua_isnil(L, 3)) {
		li_environment_remove(env, ckey, keylen);
		return 0;
	}

	if (NULL == (cval = lua_tolstring(L, 3, &vallen))) return 0;

	li_environment_set(env, ckey, keylen, cval, vallen);
	return 0;
}

static int lua_environment_unset(lua_State *L) {
	liEnvironment *env;
	const char *ckey;
	size_t keylen;

	luaL_checkany(L, 2);
	if (NULL == (env = li_lua_get_environment(L, 1))) return 0;
	if (NULL == (ckey = lua_tolstring(L, 2, &keylen))) return 0;
	li_environment_remove(env, ckey, keylen);

	return 0;
}

static int lua_environment_weak_set(lua_State *L) {
	liEnvironment *env;
	const char *ckey, *cval;
	size_t keylen, vallen;

	luaL_checkany(L, 3);
	if (NULL == (env = li_lua_get_environment(L, 1))) return 0;
	if (NULL == (ckey = lua_tolstring(L, 2, &keylen))) return 0;
	if (NULL == (cval = lua_tolstring(L, 3, &vallen))) return 0;

	li_environment_insert(env, ckey, keylen, cval, vallen);
	return 0;
}

static int lua_environment_clear(lua_State *L) {
	liEnvironment *env;

	luaL_checkany(L, 1);
	if (NULL == (env = li_lua_get_environment(L, 1))) return 0;

	li_environment_reset(env);

	return 0;
}

static int lua_environment_pairs(lua_State *L) {
	liEnvironment *env;

	luaL_checkany(L, 1);
	env = li_lua_get_environment(L, 1);

	if (!env) return 0;
	return li_lua_ghashtable_gstring_pairs(L, env->table);
}

static const luaL_Reg environment_mt[] = {
	{ "__index", lua_environment_index },
	{ "get", lua_environment_get },
	{ "__newindex", lua_environment_set },
	{ "set", lua_environment_set },
	{ "unset", lua_environment_unset },
	{ "weak_set", lua_environment_weak_set },
	{ "__pairs", lua_environment_pairs },
	{ "pairs", lua_environment_pairs },
	{ "clear", lua_environment_clear },

	{ NULL, NULL }
};

static HEDLEY_NEVER_INLINE void init_env_mt(lua_State *L) {
	luaL_register(L, NULL, environment_mt);
}

static void lua_push_environment_metatable(lua_State *L) {
	if (luaL_newmetatable(L, LUA_ENVIRONMENT)) {
		init_env_mt(L);
	}
}

void li_lua_init_environment_mt(lua_State *L) {
	lua_push_environment_metatable(L);
	lua_pop(L, 1);
}

liEnvironment* li_lua_get_environment(lua_State *L, int ndx) {
	if (!lua_isuserdata(L, ndx)) return NULL;
	if (!lua_getmetatable(L, ndx)) return NULL;
	luaL_getmetatable(L, LUA_ENVIRONMENT);
	if (lua_isnil(L, -1) || lua_isnil(L, -2) || !lua_equal(L, -1, -2)) {
		lua_pop(L, 2);
		return NULL;
	}
	lua_pop(L, 2);
	return *(liEnvironment**) lua_touserdata(L, ndx);
}

int li_lua_push_environment(lua_State *L, liEnvironment *env) {
	liEnvironment **penv;

	if (NULL == env) {
		lua_pushnil(L);
		return 1;
	}

	penv = (liEnvironment**) lua_newuserdata(L, sizeof(liEnvironment*));
	*penv = env;

	lua_push_environment_metatable(L);
	lua_setmetatable(L, -2);
	return 1;
}


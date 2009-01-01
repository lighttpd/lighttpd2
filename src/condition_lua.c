
#include <lighttpd/condition_lua.h>
#include <lighttpd/value_lua.h>

#include <lualib.h>
#include <lauxlib.h>

#define LUA_CONDITION "condition*"
#define LUA_COND_LVALUE "cond_lvalue*"
#define LUA_COND_LVALUE_T "cond_lvalue_t"

/* helper */
#define lua_mt_register_srv(srv, L, name, func) do {\
	lua_pushlightuserdata(L, srv); \
	lua_pushcclosure(L, func, 1); \
	lua_setfield(L, -2, name); \
} while(0)

#define lua_mt_register_cmp(srv, L, name, func, cmp) do {\
	lua_pushlightuserdata(L, srv); \
	lua_pushinteger(L, cmp); \
	lua_pushcclosure(L, func, 2); \
	lua_setfield(L, -2, name); \
} while(0)

#define lua_mt_register(L, name, func) do {\
	lua_pushcclosure(L, func, 0); \
	lua_setfield(L, -2, name); \
} while(0)

/* save the top of the stack in dicts described by a '.' separated path */
static void lua_settop_in_dicts(lua_State *L, const gchar *path) {
	int ndx, curtable;
	gchar** segments;
	size_t i;

	ndx = lua_gettop(L);
	segments = g_strsplit(path, ".", 10);
	assert(segments[0]);
	for (i = 0, curtable = LUA_GLOBALSINDEX; segments[i+1]; i++) {
		lua_getfield(L, curtable, segments[i]);
		if (lua_isnil(L, -1) || !lua_istable(L, -1)) {
			lua_pop(L, 1); /* pop nil */
			lua_newtable(L);
			lua_pushvalue(L, -1); /* save table in field */
			lua_setfield(L, curtable, segments[i]);
		}
		curtable = lua_gettop(L);
	}
	lua_pushvalue(L, ndx);
	lua_setfield(L, curtable, segments[i]);
	lua_pop(L, lua_gettop(L) - ndx + 1);
	g_strfreev(segments);
}

/* Get objects from lua */

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

static condition_lvalue* lua_get_condition_lvalue(lua_State *L, int ndx) {
	if (!lua_isuserdata(L, ndx)) return NULL;
	if (!lua_getmetatable(L, ndx)) return NULL;
	luaL_getmetatable(L, LUA_COND_LVALUE);
	if (lua_isnil(L, -1) || lua_isnil(L, -2) || !lua_equal(L, -1, -2)) {
		lua_pop(L, 2);
		return NULL;
	}
	lua_pop(L, 2);
	return *(condition_lvalue**) lua_touserdata(L, ndx);
}

static cond_lvalue_t lua_get_cond_lvalue_t(lua_State *L, int ndx) {
	if (!lua_isuserdata(L, ndx)) return -1;
	if (!lua_getmetatable(L, ndx)) return -1;
	luaL_getmetatable(L, LUA_COND_LVALUE_T);
	if (lua_isnil(L, -1) || lua_isnil(L, -2) || !lua_equal(L, -1, -2)) {
		lua_pop(L, 2);
		return -1;
	}
	lua_pop(L, 2);
	return *(cond_lvalue_t*) lua_touserdata(L, ndx);
}

/* Garbage collection */

static int lua_condition_gc(lua_State *L) {
	server *srv;
	condition **c = (condition**) luaL_checkudata(L, 1, LUA_CONDITION);
	if (!c || !*c) return 0;

	srv = (server*) lua_touserdata(L, lua_upvalueindex(1));
	condition_release(srv, *c);
	return 0;
}

static int lua_cond_lvalue_gc(lua_State *L) {
	condition_lvalue **lvalue = (condition_lvalue**) luaL_checkudata(L, 1, LUA_COND_LVALUE);
	if (!lvalue || !*lvalue) return 0;

	condition_lvalue_release(*lvalue);
	return 0;
}

/* new metatables and push */

static void lua_push_condition_metatable(server *srv, lua_State *L) {
	if (luaL_newmetatable(L, LUA_CONDITION)) {
		lua_mt_register_srv(srv, L, "__gc", lua_condition_gc);
	}
}

int lua_push_condition(server *srv, lua_State *L, condition *c) {
	condition **pc;

	pc = (condition**) lua_newuserdata(L, sizeof(condition*));
	*pc = c;

	lua_push_condition_metatable(srv, L);

	lua_setmetatable(L, -2);
	return 1;
}


/* cond_lvalue metatable (except __gc) */

/* ::_tostring */
static int lua_cond_lvalue_tostring(lua_State *L) {
	condition_lvalue *lvalue = lua_get_condition_lvalue(L, 1);
	if (!lvalue) return 0;
	lua_pushstring(L, cond_lvalue_to_string(lvalue->type));
	if (lvalue->key) {
		lua_pushstring(L, "['");
		lua_pushstring(L, lvalue->key->str);
		lua_pushstring(L, "']");
		lua_concat(L, 4);
	}
	return 1;
}

static int lua_cond_lvalue_cmp(lua_State *L) {
	server *srv;
	GString *sval;
	condition *c;
	condition_lvalue *lvalue;
	comp_operator_t cmpop;

	lvalue = lua_get_condition_lvalue(L, 1);
	srv = (server*) lua_touserdata(L, lua_upvalueindex(1));
	cmpop = (comp_operator_t) lua_tointeger(L, lua_upvalueindex(2));

	if (NULL == (sval = lua_togstring(L, 2))) return 0;
	c = condition_new_string(srv, cmpop, lvalue, sval);
	if (c) {
		condition_lvalue_acquire(lvalue);
		lua_push_condition(srv, L, c);
		return 1;
	}
	return 0;
}

static void lua_push_cond_lvalue_metatable(server *srv, lua_State *L) {
	if (luaL_newmetatable(L, LUA_COND_LVALUE)) {
		lua_mt_register(L, "__gc", lua_cond_lvalue_gc);
		lua_mt_register(L, "__tostring", lua_cond_lvalue_tostring);

		lua_mt_register_cmp(srv, L, "eq", lua_cond_lvalue_cmp, CONFIG_COND_EQ);
		lua_mt_register_cmp(srv, L, "ne", lua_cond_lvalue_cmp, CONFIG_COND_NE);
		lua_mt_register_cmp(srv, L, "prefix", lua_cond_lvalue_cmp, CONFIG_COND_PREFIX);
		lua_mt_register_cmp(srv, L, "notprefix", lua_cond_lvalue_cmp, CONFIG_COND_NOPREFIX);
		lua_mt_register_cmp(srv, L, "suffix", lua_cond_lvalue_cmp, CONFIG_COND_SUFFIX);
		lua_mt_register_cmp(srv, L, "notsuffix", lua_cond_lvalue_cmp, CONFIG_COND_NOSUFFIX);
		lua_mt_register_cmp(srv, L, "match", lua_cond_lvalue_cmp, CONFIG_COND_MATCH);
		lua_mt_register_cmp(srv, L, "nomatch", lua_cond_lvalue_cmp, CONFIG_COND_NOMATCH);
		lua_mt_register_cmp(srv, L, "ip", lua_cond_lvalue_cmp, CONFIG_COND_IP);
		lua_mt_register_cmp(srv, L, "notip", lua_cond_lvalue_cmp, CONFIG_COND_NOTIP);
		lua_mt_register_cmp(srv, L, "gt", lua_cond_lvalue_cmp, CONFIG_COND_GT);
		lua_mt_register_cmp(srv, L, "ge", lua_cond_lvalue_cmp, CONFIG_COND_GE);
		lua_mt_register_cmp(srv, L, "lt", lua_cond_lvalue_cmp, CONFIG_COND_LT);
		lua_mt_register_cmp(srv, L, "le", lua_cond_lvalue_cmp, CONFIG_COND_LE);

		lua_pushvalue(L, -1);
		lua_setfield(L, -2, "__index");
	}
}

static int lua_push_cond_lvalue(server *srv, lua_State *L, condition_lvalue *lvalue) {
	condition_lvalue **pv;

	pv = (condition_lvalue**) lua_newuserdata(L, sizeof(condition_lvalue*));
	*pv = lvalue;

	lua_push_cond_lvalue_metatable(srv, L);

	lua_setmetatable(L, -2);
	return 1;
}

/* cond_lvalue_t metatable */
static int lua_cond_lvalue_t_tostring(lua_State *L) {
	cond_lvalue_t t = lua_get_cond_lvalue_t(L, 1);
	lua_pushstring(L, cond_lvalue_to_string(t));
	return 1;
}

static int lua_cond_lvalue_t_index(lua_State *L) {
	server *srv;
	GString *key;
	cond_lvalue_t t = lua_get_cond_lvalue_t(L, 1);

	srv = (server*) lua_touserdata(L, lua_upvalueindex(1));
	if (t < COND_LVALUE_FIRST_WITH_KEY || t >= COND_LVALUE_END) return 0;
	if (NULL == (key = lua_togstring(L, 2))) return 0;
	lua_push_cond_lvalue(srv, L, condition_lvalue_new(t, key));
	return 1;
}

static void lua_push_cond_lvalue_t_metatable(server *srv, lua_State *L) {
	if (luaL_newmetatable(L, LUA_COND_LVALUE_T)) {
		lua_mt_register(L, "__tostring", lua_cond_lvalue_t_tostring);
		lua_mt_register_srv(srv, L, "__index", lua_cond_lvalue_t_index);
	}
}

/* cond_lvalue_t */

static int lua_push_cond_lvalue_t(server *srv, lua_State *L, cond_lvalue_t t) {
	cond_lvalue_t *pt;

	pt = (cond_lvalue_t*) lua_newuserdata(L, sizeof(cond_lvalue_t));
	*pt = t;

	lua_push_cond_lvalue_t_metatable(srv, L);

	lua_setmetatable(L, -2);
	return 1;
}




void lua_push_lvalues_dict(server *srv, lua_State *L) {
	size_t i;

	for (i = 0; i < COND_LVALUE_FIRST_WITH_KEY; i++) {
		lua_push_cond_lvalue(srv, L, condition_lvalue_new(i, NULL));
		lua_settop_in_dicts(L, cond_lvalue_to_string(i));
	}

	for ( ; i < COND_LVALUE_END; i++) {
		lua_push_cond_lvalue_t(srv, L, i);
		lua_settop_in_dicts(L, cond_lvalue_to_string(i));
	}
}

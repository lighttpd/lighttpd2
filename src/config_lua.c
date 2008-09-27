
#include "config_lua.h"
#include "condition_lua.h"
#include "value_lua.h"
#include "actions_lua.h"

#include <lualib.h>
#include <lauxlib.h>

typedef int (*LuaWrapper)(server *srv, lua_State *L, gpointer data);

static value* lua_params_to_value(server *srv, lua_State *L) {
	value *val, *subval;
	switch (lua_gettop(L)) {
	case 0:
		return NULL;
	case 1:
		return value_from_lua(srv, L);
	default:
		val = value_new_list();
		g_array_set_size(val->data.list, lua_gettop(L));
		while (lua_gettop(L) > 0) {
			if (NULL == (subval = value_from_lua(srv, L))) {
				ERROR(srv, "%s", "Couldn't convert value from lua");
				value_free(val);
				return NULL;
			}
			g_array_index(val->data.list, value*, lua_gettop(L)) = subval;
		}
		return val;
	}
	return NULL;
}

static int lua_str_hash_index(lua_State *L) {
	server *srv;
	GHashTable *ht;
	LuaWrapper wrapper;
	const char *key;
	gpointer d;

	srv = (server*) lua_touserdata(L, lua_upvalueindex(1));
	ht = (GHashTable*) lua_touserdata(L, lua_upvalueindex(2));
	wrapper = (LuaWrapper)(intptr_t) lua_touserdata(L, lua_upvalueindex(3));
	key = luaL_checklstring(L, 2, NULL);

	if (key && NULL != (d = g_hash_table_lookup(ht, key))) {
		lua_pop(L, lua_gettop(L));
		return wrapper(srv, L, d);
	}

	lua_pop(L, lua_gettop(L));
	lua_pushnil(L);
	return 1;
}

/* Creates a table on the lua stack */
static gboolean publish_str_hash(server *srv, lua_State *L, GHashTable *ht, LuaWrapper wrapper) {
	lua_newtable(L);                   /* { } */
	lua_newtable(L);                   /* metatable */

	lua_pushlightuserdata(L, srv);
	lua_pushlightuserdata(L, ht);
	lua_pushlightuserdata(L, (void*)(intptr_t)wrapper);
	lua_pushcclosure(L, lua_str_hash_index, 3);

	lua_setfield(L, -2, "__index");
	lua_setmetatable(L, -2);
	return TRUE;
}

static int handle_server_action(lua_State *L) {
	server *srv;
	server_action *sa;
	value *val;
	action *a;

	srv = (server*) lua_touserdata(L, lua_upvalueindex(1));
	sa = (server_action*) lua_touserdata(L, lua_upvalueindex(2));

	val = lua_params_to_value(srv, L);

	/* TRACE(srv, "%s", "Creating action"); */
	a = sa->create_action(srv, sa->p, val);
	value_free(val);

	if (NULL == a) {
		lua_pushstring(L, "creating action failed");
		lua_error(L);
	}

	return lua_push_action(srv, L, a);
}

static int wrap_server_action(server *srv, lua_State *L, gpointer sa) {
	lua_pushlightuserdata(L, srv);
	lua_pushlightuserdata(L, sa);
	lua_pushcclosure(L, handle_server_action, 2);
	return 1;
}

static int handle_server_setup(lua_State *L) {
	server *srv;
	server_setup *ss;
	value *val;

	srv = (server*) lua_touserdata(L, lua_upvalueindex(1));
	ss = (server_setup*) lua_touserdata(L, lua_upvalueindex(2));

	val = lua_params_to_value(srv, L);

	/* TRACE(srv, "%s", "Calling setup"); */

	if (!ss->setup(srv, ss->p, val)) {
		value_free(val);
		lua_pushstring(L, "setup failed");
		lua_error(L);
	}

	value_free(val);
	return 0;
}

static int wrap_server_setup(server *srv, lua_State *L, gpointer ss) {
	lua_pushlightuserdata(L, srv);
	lua_pushlightuserdata(L, ss);
	lua_pushcclosure(L, handle_server_setup, 2);
	return 1;
}

gboolean config_lua_load(server *srv, const gchar *filename) {
	lua_State *L;

	L = luaL_newstate();
	luaL_openlibs(L);

	if (0 != luaL_loadfile(L, filename)) {
		ERROR(srv, "Loading script '%s' failed: %s", filename, lua_tostring(L, -1));
		return FALSE;
	}

	TRACE(srv, "Loaded config script '%s'", filename);

	publish_str_hash(srv, L, srv->setups, wrap_server_setup);
	lua_setfield(L, LUA_GLOBALSINDEX, "setup");

	publish_str_hash(srv, L, srv->actions, wrap_server_action);
	lua_setfield(L, LUA_GLOBALSINDEX, "action");

	lua_push_lvalues_dict(srv, L);

	if (lua_pcall(L, 0, 1, 0)) {
		ERROR(srv, "lua_pcall(): %s", lua_tostring(L, -1));
		return FALSE;
	}

	lua_pop(L, 1); /* pop the ret-value */

	lua_getfield(L, LUA_GLOBALSINDEX, "actions");
	srv->mainaction = lua_get_action(L, -1);
	action_acquire(srv->mainaction);
	lua_pop(L, 1);

	assert(lua_gettop(L) == 0);

	lua_close(L);
	return TRUE;
}

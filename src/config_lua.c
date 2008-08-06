
#include "config_lua.h"
#include "condition_lua.h"
#include "options_lua.h"
#include "actions_lua.h"

#include <lualib.h>
#include <lauxlib.h>

typedef int (*LuaWrapper)(server *srv, lua_State *L, gpointer data);

static option* lua_params_to_option(server *srv, lua_State *L) {
	option *opt, *subopt;
	switch (lua_gettop(L)) {
	case 0:
		return NULL;
	case 1:
		return option_from_lua(srv, L);
	default:
		opt = option_new_list();
		while (lua_gettop(L) > 0) {
			if (NULL == (subopt = option_from_lua(srv, L))) {
				ERROR(srv, "%s", "Couldn't convert option to lua");
				option_free(opt);
				return NULL;
			}
			g_array_append_val(opt->value.opt_list, subopt);
		}
		return opt;
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
	option *opt;
	action *a;

	srv = (server*) lua_touserdata(L, lua_upvalueindex(1));
	sa = (server_action*) lua_touserdata(L, lua_upvalueindex(2));

	opt = lua_params_to_option(srv, L);

	TRACE(srv, "%s", "Creating action");

	if (NULL == (a = sa->create_action(srv, sa->p, opt))) {
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
	option *opt;

	srv = (server*) lua_touserdata(L, lua_upvalueindex(1));
	ss = (server_setup*) lua_touserdata(L, lua_upvalueindex(2));

	opt = lua_params_to_option(srv, L);

	TRACE(srv, "%s", "Calling setup");

	if (!ss->setup(srv, ss->p, opt)) {
		lua_pushstring(L, "setup failed");
		lua_error(L);
	}

	return 0;
}

static int wrap_server_setup(server *srv, lua_State *L, gpointer ss) {
	lua_pushlightuserdata(L, srv);
	lua_pushlightuserdata(L, ss);
	lua_pushcclosure(L, handle_server_setup, 2);
	return 1;
}

static action* action_from_lua(server *srv, lua_State *L) {
	const gchar *optname;
	option *value;
	action *a;

	optname = luaL_checklstring(L, -2, NULL);
	if (!optname) {
		lua_pushstring(L, "wrong config argument, expected string");
		lua_error(L);
	}
	value = option_from_lua(srv, L);
	if (!value) {
		lua_pushstring(L, "missing config value");
		lua_error(L);
	}
	a = option_action(srv, optname, value);
	if (!a) {
		option_free(value);
		lua_pushstring(L, "couldn't create action from setting");
		lua_error(L);
	}
	return a;
}

static int handle_option(lua_State *L) {
	server *srv;
	action *a, *suba;

	srv = (server*) lua_touserdata(L, lua_upvalueindex(1));
	switch (lua_gettop(L)) {
	case 1:
		if (!lua_istable(L, 1)) {
			lua_pushstring(L, "wrong config argument, expected table");
			lua_error(L);
		}
		a = action_new_list();
		lua_push_action(srv, L, a);
		lua_insert(L, 1);
		lua_pushnil(L);
		while (lua_next(L, 2) != 0) {
			suba = action_from_lua(srv, L);
			g_array_append_val(a->value.list, suba);
			lua_pop(L, 1);
		}
		lua_pop(L, 1);
		return 1;
	case 2:
		a = action_from_lua(srv, L);
		return lua_push_action(srv, L, a);
	default:
		lua_pushstring(L, "wrong count of arguments to config()");
		lua_error(L);
	}
	return 0;
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

	lua_pushlightuserdata(L, srv);
	lua_pushcclosure(L, handle_option, 1);
	lua_setfield(L, LUA_GLOBALSINDEX, "option");

	lua_push_lvalues_dict(srv, L);

	if (lua_pcall(L, 0, 1, 0)) {
		ERROR(srv, "lua_pcall(): %s", lua_tostring(L, -1));
		return FALSE;
	}

	lua_pop(L, 1); /* pop the ret-value */

	lua_getfield(L, LUA_GLOBALSINDEX, "actions");
	srv->mainaction = lua_get_action(L, -1);
	lua_pop(L, 1);

	assert(lua_gettop(L) == 0);

	lua_close(L);
	return TRUE;
}

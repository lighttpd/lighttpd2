
#include <lighttpd/config_lua.h>
#include <lighttpd/condition_lua.h>
#include <lighttpd/value_lua.h>
#include <lighttpd/actions_lua.h>

#include <lighttpd/core_lua.h>

#include <lualib.h>
#include <lauxlib.h>

typedef int (*LuaWrapper)(liServer *srv, liWorker *wrk, lua_State *L, const char *key);

static liValue* lua_params_to_value(liServer *srv, lua_State *L) {
	liValue *val, *subval;
	switch (lua_gettop(L)) {
	case 0:
	case 1: /* first parameter is the table the __call method is for */
		return NULL;
	case 2:
		return li_value_from_lua(srv, L);
	default:
		val = li_value_new_list();
		g_array_set_size(val->data.list, lua_gettop(L) - 1);
		while (lua_gettop(L) > 1) {
			subval = li_value_from_lua(srv, L);
			g_array_index(val->data.list, liValue*, lua_gettop(L) - 1) = subval;
		}
		return val;
	}
	return NULL;
}

static int _lua_dynamic_hash_index_call(lua_State *L) {
	liServer *srv;
	liWorker *wrk;
	LuaWrapper wrapper;
	const char *key;

	srv = (liServer*) lua_touserdata(L, lua_upvalueindex(1));
	wrk = (liWorker*) lua_touserdata(L, lua_upvalueindex(2));
	wrapper = (LuaWrapper)(intptr_t) lua_touserdata(L, lua_upvalueindex(3));
	key = luaL_checklstring(L, lua_upvalueindex(4), NULL);

	return wrapper(srv, wrk, L, key);
}

/* downside of this dynamic hash: you always get a function back on __index,
 * no matter whether the key actually exists
 */
static int _lua_dynamic_hash_index(lua_State *L) {
	int key_ndx;

	lua_pushvalue(L, lua_upvalueindex(4));
	lua_pushvalue(L, 2);
	lua_concat(L, 2);
	key_ndx = lua_gettop(L);

	lua_newtable(L); /* result */
	lua_newtable(L); /* meta table */

	/* call method */
	lua_pushvalue(L, lua_upvalueindex(1));
	lua_pushvalue(L, lua_upvalueindex(2));
	lua_pushvalue(L, lua_upvalueindex(3));
	lua_pushvalue(L, key_ndx);
	lua_pushcclosure(L, _lua_dynamic_hash_index_call, 4);
	lua_setfield(L, -2, "__call");

	/* index for "nested" keys */
	lua_pushvalue(L, lua_upvalueindex(1));
	lua_pushvalue(L, lua_upvalueindex(2));
	lua_pushvalue(L, lua_upvalueindex(3));
	lua_pushvalue(L, key_ndx); /* append a "." to the current key for nesting */
	lua_pushstring(L, ".");
	lua_concat(L, 2);
	lua_pushcclosure(L, _lua_dynamic_hash_index, 4);
	lua_setfield(L, -2, "__index");

	lua_setmetatable(L, -2);

	return 1;
}

static void lua_push_dynamic_hash(liServer *srv, liWorker *wrk, lua_State *L, LuaWrapper wrapper) {
	lua_newtable(L);
	lua_newtable(L); /* meta table */
	lua_pushlightuserdata(L, srv);
	lua_pushlightuserdata(L, wrk);
	lua_pushlightuserdata(L, (void*)(intptr_t) wrapper);
	lua_pushstring(L, ""); /* nesting starts at "root" with empty string */
	lua_pushcclosure(L, _lua_dynamic_hash_index, 4);
	lua_setfield(L, -2, "__index");
	lua_setmetatable(L, -2);
}

static int li_lua_config_handle_server_action(liServer *srv, liWorker *wrk, lua_State *L, const char *key) {
	liAction *a;
	liValue *val;
	liLuaState *LL = li_lua_state_get(L);

	lua_checkstack(L, 16);
	val = lua_params_to_value(srv, L);

	li_lua_unlock(LL);

	a = li_plugin_config_action(srv, wrk, key, val);

	li_lua_lock(LL);

	if (NULL == a) {
		lua_pushstring(L, "creating action failed");
		lua_error(L);
	}

	return li_lua_push_action(srv, L, a);
}

void li_lua_push_action_table(liServer *srv, liWorker *wrk, lua_State *L) {
	lua_push_dynamic_hash(srv, wrk, L, li_lua_config_handle_server_action);
}

static int li_lua_config_handle_server_setup(liServer *srv, liWorker *wrk, lua_State *L, const char *key) {
	gboolean result;
	liValue *val;
	liLuaState *LL = li_lua_state_get(L);
	assert(srv->main_worker == wrk);

	lua_checkstack(L, 16);
	val = lua_params_to_value(srv, L);

	li_lua_unlock(LL);

	result = li_plugin_config_setup(srv, key, val);

	li_lua_lock(LL);

	if (!result) {
		lua_pushstring(L, "setup failed");
		lua_error(L);
	}

	return 0;
}

void li_lua_push_setup_table(liServer *srv, liWorker *wrk, lua_State *L) {
	assert(srv->main_worker == wrk);
	lua_push_dynamic_hash(srv, wrk, L, li_lua_config_handle_server_setup);
}

gboolean li_config_lua_load(liLuaState *LL, liServer *srv, liWorker *wrk, const gchar *filename, liAction **pact, gboolean allow_setup, liValue *args) {
	int errfunc;
	int lua_stack_top;
	lua_State *L = LL->L;

	*pact = NULL;

	li_lua_lock(LL);

	lua_stack_top = lua_gettop(L);

	li_lua_new_globals(L);

	if (0 != luaL_loadfile(L, filename)) {
		_ERROR(srv, wrk, NULL, "Loading script '%s' failed: %s", filename, lua_tostring(L, -1));
		return FALSE;
	}

	_DEBUG(srv, wrk, NULL, "Loaded config script '%s'", filename);

	if (allow_setup) {
		assert(wrk == srv->main_worker);
		li_lua_push_setup_table(srv, wrk, L);
		lua_setfield(L, LUA_GLOBALSINDEX, "setup");
	}

	li_lua_push_action_table(srv, wrk, L);
	lua_setfield(L, LUA_GLOBALSINDEX, "action");

	li_lua_push_lvalues_dict(srv, L);

	/* arguments for config: local filename, args = ... */
	/* 1. filename */
	lua_pushstring(L, filename);
	/* 2. args */
	li_lua_push_value(L, args);

	errfunc = li_lua_push_traceback(L, 2);
	if (lua_pcall(L, 2, 0, errfunc)) {
		_ERROR(srv, wrk, NULL, "lua_pcall(): %s", lua_tostring(L, -1));

		/* cleanup stack */
		if (lua_stack_top > lua_gettop(L)) {
			lua_pop(L, lua_gettop(L) - lua_stack_top);
		}

		li_lua_restore_globals(L);

		li_lua_unlock(LL);

		return FALSE;
	}
	lua_remove(L, errfunc);

	lua_getfield(L, LUA_GLOBALSINDEX, "actions");
	*pact = li_lua_get_action_ref(L, -1);
	lua_pop(L, 1);

	assert(lua_gettop(L) == lua_stack_top);

	li_lua_restore_globals(L);

	lua_gc(L, LUA_GCCOLLECT, 0);

	li_lua_unlock(LL);

	return TRUE;
}

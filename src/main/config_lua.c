
#include <lighttpd/config_lua.h>
#include <lighttpd/condition_lua.h>
#include <lighttpd/value_lua.h>
#include <lighttpd/actions_lua.h>

#include <lighttpd/core_lua.h>

typedef int (*LuaWrapper)(liServer *srv, liWorker *wrk, lua_State *L, const char *key);

static liValue* lua_params_to_value(liServer *srv, lua_State *L) {
	liValue *val, *subval;
	switch (lua_gettop(L)) {
	case 0:
	case 1: /* first parameter is the table the __call method is for */
		return NULL;
	default:
		val = li_value_new_list();
		g_ptr_array_set_size(val->data.list, lua_gettop(L) - 1);
		while (lua_gettop(L) > 1) {
			subval = li_value_from_lua(srv, L);
			g_ptr_array_index(val->data.list, lua_gettop(L) - 1) = subval;
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

	/* upvalues: 1: srv, 2: wrk, 3: "wrapper", 4: key prefix */

	/* concat key prefix and new sub key */
	lua_pushvalue(L, lua_upvalueindex(4)); /* key prefix */
	lua_pushvalue(L, 2);
	lua_concat(L, 2);
	key_ndx = lua_gettop(L);

	lua_newuserdata(L, 0); /* result: zero-sized userdata object */
	lua_newtable(L); /* meta table */
	li_lua_protect_metatable(L);

	/* call method */
	lua_pushvalue(L, lua_upvalueindex(1)); /* srv */
	lua_pushvalue(L, lua_upvalueindex(2)); /* wrk */
	lua_pushvalue(L, lua_upvalueindex(3)); /* wrapper */
	lua_pushvalue(L, key_ndx);
	lua_pushcclosure(L, _lua_dynamic_hash_index_call, 4);
	lua_setfield(L, -2, "__call");

	/* index for "nested" keys */
	lua_pushvalue(L, lua_upvalueindex(1)); /* srv */
	lua_pushvalue(L, lua_upvalueindex(2)); /* wrk */
	lua_pushvalue(L, lua_upvalueindex(3)); /* wrapper */
	lua_pushvalue(L, key_ndx); /* append a "." to the current key for nesting */
	lua_pushstring(L, ".");
	lua_concat(L, 2);
	lua_pushcclosure(L, _lua_dynamic_hash_index, 4);
	lua_setfield(L, -2, "__index");

	lua_setmetatable(L, -2);

	return 1;
}

static void lua_push_dynamic_hash(liServer *srv, liWorker *wrk, lua_State *L, LuaWrapper wrapper) {
	lua_newuserdata(L, 0); /* result: zero-sized userdata object */
	lua_newtable(L); /* meta table */
	li_lua_protect_metatable(L);

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

	if (!wrk) wrk = srv->main_worker;
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
	LI_FORCE_ASSERT(srv->main_worker == wrk);

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
	LI_FORCE_ASSERT(srv->main_worker == wrk);
	lua_push_dynamic_hash(srv, wrk, L, li_lua_config_handle_server_setup);
}

gboolean li_config_lua_load(liLuaState *LL, liServer *srv, liWorker *wrk, const gchar *filename, liAction **pact, gboolean allow_setup, liValue *args) {
	int errfunc;
	int lua_stack_top;
	lua_State *L = LL->L;
	gboolean result;

	*pact = NULL;

	li_lua_lock(LL);

	li_lua_environment_use_globals(LL); /* +1 */
	li_lua_environment_activate_ephemeral(LL); /* +1 */
	lua_stack_top = lua_gettop(L);

	if (0 != luaL_loadfile(L, filename)) { /* +1 lua script to run */
		_ERROR(srv, wrk, NULL, "Loading script '%s' failed: %s", filename, lua_tostring(L, -1));
		return FALSE;
	}

	_DEBUG(srv, wrk, NULL, "Loaded config script '%s'", filename);

	if (allow_setup) {
		LI_FORCE_ASSERT(wrk == srv->main_worker);
		li_lua_push_setup_table(srv, wrk, L); /* +1 */
		lua_setglobal(L, "setup"); /* -1 */
	}

	/* arguments for config: local filename, args = ... */
	/* 1. filename */
	lua_pushstring(L, filename); /* +1 */
	/* 2. args */
	li_lua_push_value(L, args); /* +1 */

	errfunc = li_lua_push_traceback(L, 2); /* +1, but before func and 2 args */
	if (lua_pcall(L, 2, 0, errfunc)) { /* -3 (func + args), 0 results (but 1 error) */
		_ERROR(srv, wrk, NULL, "lua_pcall(): %s", lua_tostring(L, -1));

		lua_pop(L, 1); /* -1 error */

		result = FALSE;
	} else {
		lua_getglobal(L, "actions"); /* +1 */
		*pact = li_lua_get_action_ref(L, -1);
		lua_pop(L, 1); /* -1 */

		result = TRUE;
	}
	lua_remove(L, errfunc); /* -1 */

	LI_FORCE_ASSERT(lua_gettop(L) == lua_stack_top);

	li_lua_environment_restore(LL); /* -1 */
	li_lua_environment_restore_globals(L); /* -1 */

	lua_gc(L, LUA_GCCOLLECT, 0);

	li_lua_unlock(LL);

	return result;
}

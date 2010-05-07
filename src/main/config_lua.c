
#include <lighttpd/config_lua.h>
#include <lighttpd/condition_lua.h>
#include <lighttpd/value_lua.h>
#include <lighttpd/actions_lua.h>

#include <lighttpd/core_lua.h>

#include <lualib.h>
#include <lauxlib.h>

typedef int (*LuaWrapper)(liServer *srv, liWorker *wrk, lua_State *L, gpointer data);

static liValue* lua_params_to_value(liServer *srv, lua_State *L) {
	liValue *val, *subval;
	switch (lua_gettop(L)) {
	case 0:
	case 1:
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

/* Creates a table on the lua stack */
static void lua_push_publish_hash_metatable(liServer *srv, liWorker *wrk, lua_State *L);

static int lua_str_hash_index(lua_State *L) {
	liServer *srv;
	liWorker *wrk;
	GHashTable *ht;
	LuaWrapper wrapper;

	srv = (liServer*) lua_touserdata(L, lua_upvalueindex(1));
	wrk = (liWorker*) lua_touserdata(L, lua_upvalueindex(2));
	lua_pushstring(L, "__ht"); lua_rawget(L, 1);
	ht = (GHashTable*) lua_touserdata(L, -1); lua_pop(L, 1);
	lua_pushstring(L, "__wrapper"); lua_rawget(L, 1);
	wrapper = (LuaWrapper)(intptr_t) lua_touserdata(L, -1); lua_pop(L, 1);
	if (!lua_isstring(L, 2) || !ht || !wrapper) {
		lua_pop(L, lua_gettop(L));
		lua_pushstring(L, "lookup failed (lua_str_hash_index)");
		lua_error(L);
		return 0;
	}

	/* TRACE(srv, "str hash index: '%s'", luaL_checklstring(L, 2, NULL)); */

	lua_newtable(L);
	lua_pushlightuserdata(L, ht);
	lua_setfield(L, -2, "__ht");
	lua_pushlightuserdata(L, (void*)(intptr_t)wrapper);
	lua_setfield(L, -2, "__wrapper");

	lua_pushstring(L, "__key"); lua_rawget(L, 1);
	if (lua_isstring(L, -1)) {
		lua_pushstring(L, ".");
		lua_pushvalue(L, 2);
		lua_concat(L, 3);
	} else {
		lua_pop(L, 1);
		lua_pushvalue(L, 2);
	}
	lua_setfield(L, -2, "__key");
	
	lua_push_publish_hash_metatable(srv, wrk, L);
	lua_setmetatable(L, -2);
	return 1;
}

static int lua_str_hash_call(lua_State *L) {
	liServer *srv;
	liWorker *wrk;
	GHashTable *ht;
	LuaWrapper wrapper;
	const char *key;
	gpointer d;

	srv = (liServer*) lua_touserdata(L, lua_upvalueindex(1));
	wrk = (liWorker*) lua_touserdata(L, lua_upvalueindex(2));
	lua_pushstring(L, "__ht"); lua_rawget(L, 1);
	ht = (GHashTable*) lua_touserdata(L, -1); lua_pop(L, 1);
	lua_pushstring(L, "__wrapper"); lua_rawget(L, 1);
	wrapper = (LuaWrapper)(intptr_t) lua_touserdata(L, -1); lua_pop(L, 1);
	lua_pushstring(L, "__key"); lua_rawget(L, 1);
	key = luaL_checklstring(L, -1, NULL);

	/* DEBUG(srv, "str hash call: '%s'", key); */

	if (key && NULL != (d = g_hash_table_lookup(ht, key))) {
		lua_pop(L, 1);
		return wrapper(srv, wrk, L, d);
	}

	lua_pop(L, lua_gettop(L));
	lua_pushstring(L, "lookup '");
	lua_pushstring(L, key);
	lua_pushstring(L, "' failed (lua_str_hash_call)");
	lua_concat(L, 3);
	lua_error(L);
	return 0;
}

#define LUA_PUBLISH_HASH "GHashTable*"
static void lua_push_publish_hash_metatable(liServer *srv, liWorker *wrk, lua_State *L) {
	if (luaL_newmetatable(L, LUA_PUBLISH_HASH)) {
		lua_pushlightuserdata(L, srv);
		lua_pushlightuserdata(L, wrk);
		lua_pushcclosure(L, lua_str_hash_index, 2);
		lua_setfield(L, -2, "__index");
		
		lua_pushlightuserdata(L, srv);
		lua_pushlightuserdata(L, wrk);
		lua_pushcclosure(L, lua_str_hash_call, 2);
		lua_setfield(L, -2, "__call");
	}
}

gboolean li_lua_config_publish_str_hash(liServer *srv, liWorker *wrk, lua_State *L, GHashTable *ht, LuaWrapper wrapper) {
	lua_newtable(L);                   /* { } */
	lua_pushlightuserdata(L, ht);
	lua_setfield(L, -2, "__ht");
	lua_pushlightuserdata(L, (void*)(intptr_t)wrapper);
	lua_setfield(L, -2, "__wrapper");

	lua_push_publish_hash_metatable(srv, wrk, L);
	lua_setmetatable(L, -2);
	return TRUE;
}


int li_lua_config_handle_server_action(liServer *srv, liWorker *wrk, lua_State *L, gpointer _sa) {
	liServerAction *sa = (liServerAction*) _sa;
	liValue *val;
	liAction *a;
	gboolean dolock = (L == srv->L);

	lua_checkstack(L, 16);
	val = lua_params_to_value(srv, L);

	if (dolock) li_lua_unlock(srv);

	/* TRACE(srv, "%s", "Creating action"); */
	a = sa->create_action(srv, wrk, sa->p, val, sa->userdata);
	li_value_free(val);

	if (dolock) li_lua_lock(srv);

	if (NULL == a) {
		lua_pushstring(L, "creating action failed");
		lua_error(L);
	}

	return li_lua_push_action(srv, L, a);
}

int li_lua_config_handle_server_setup(liServer *srv, liWorker *wrk, lua_State *L, gpointer _ss) {
	liServerSetup *ss = (liServerSetup*) _ss;
	liValue *val;
	gboolean res;
	gboolean dolock = (L == srv->L);
	UNUSED(wrk);

	lua_checkstack(L, 16);
	val = lua_params_to_value(srv, L);

	if (dolock) li_lua_unlock(srv);
	/* TRACE(srv, "%s", "Calling setup"); */
	res = ss->setup(srv, ss->p, val, ss->userdata);
	if (dolock) li_lua_lock(srv);

	if (!res) {
		li_value_free(val);
		lua_pushstring(L, "setup failed");
		lua_error(L);
	}

	li_value_free(val);
	return 0;
}

gboolean li_config_lua_load(lua_State *L, liServer *srv, liWorker *wrk, const gchar *filename, liAction **pact, gboolean allow_setup, liValue *args) {
	int errfunc;
	int lua_stack_top;
	gboolean dolock = (L == srv->L);

	*pact = NULL;

	if (dolock) li_lua_lock(srv);

	lua_stack_top = lua_gettop(L);

	li_lua_new_globals(L);

	if (0 != luaL_loadfile(L, filename)) {
		ERROR(srv, "Loading script '%s' failed: %s", filename, lua_tostring(L, -1));
		return FALSE;
	}

	DEBUG(srv, "Loaded config script '%s'", filename);

	if (allow_setup) {
		li_lua_config_publish_str_hash(srv, wrk, L, srv->setups, li_lua_config_handle_server_setup);
		lua_setfield(L, LUA_GLOBALSINDEX, "setup");
	}

	li_lua_config_publish_str_hash(srv, wrk, L, srv->actions, li_lua_config_handle_server_action);
	lua_setfield(L, LUA_GLOBALSINDEX, "action");

	li_lua_push_lvalues_dict(srv, L);

	/* arguments for config: local filename, args = ... */
	/* 1. filename */
	lua_pushstring(L, filename);
	/* 2. args */
	li_lua_push_value(L, args);

	errfunc = li_lua_push_traceback(L, 2);
	if (lua_pcall(L, 2, 0, errfunc)) {
		ERROR(srv, "lua_pcall(): %s", lua_tostring(L, -1));

		/* cleanup stack */
		if (lua_stack_top > lua_gettop(L)) {
			lua_pop(L, lua_gettop(L) - lua_stack_top);
		}

		li_lua_restore_globals(L);

		if (dolock) li_lua_unlock(srv);

		return FALSE;
	}
	lua_remove(L, errfunc);

	lua_getfield(L, LUA_GLOBALSINDEX, "actions");
	*pact = li_lua_get_action_ref(L, -1);
	lua_pop(L, 1);

	assert(lua_gettop(L) == lua_stack_top);

	li_lua_restore_globals(L);

	if (dolock) li_lua_unlock(srv);

	return TRUE;
}

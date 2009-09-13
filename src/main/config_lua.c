
#include <lighttpd/config_lua.h>
#include <lighttpd/condition_lua.h>
#include <lighttpd/value_lua.h>
#include <lighttpd/actions_lua.h>

# include <lighttpd/core_lua.h>

#include <lualib.h>
#include <lauxlib.h>

typedef int (*LuaWrapper)(liServer *srv, lua_State *L, gpointer data);

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
			if (NULL == (subval = li_value_from_lua(srv, L))) {
				ERROR(srv, "Couldn't convert value from lua (lua type '%s')", lua_typename(L, lua_type(L, -1)));
				li_value_free(val);
				return NULL;
			}
			g_array_index(val->data.list, liValue*, lua_gettop(L) - 1) = subval;
		}
		return val;
	}
	return NULL;
}

/* Creates a table on the lua stack */
static void lua_push_publish_hash_metatable(liServer *srv, lua_State *L);

static int lua_str_hash_index(lua_State *L) {
	liServer *srv;
	GHashTable *ht;
	LuaWrapper wrapper;

	srv = (liServer*) lua_touserdata(L, lua_upvalueindex(1));
	lua_pushstring(L, "__ht"); lua_rawget(L, 1);
	ht = (GHashTable*) lua_touserdata(L, -1); lua_pop(L, 1);
	lua_pushstring(L, "__wrapper"); lua_rawget(L, 1);
	wrapper = (LuaWrapper)(intptr_t) lua_touserdata(L, -1); lua_pop(L, 1);
	if (!lua_isstring(L, 2) || !ht || !wrapper) {
		lua_pop(L, lua_gettop(L));
		lua_pushstring(L, "lookup failed");
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
	
	lua_push_publish_hash_metatable(srv, L);
	lua_setmetatable(L, -2);
	return 1;
}

static int lua_str_hash_call(lua_State *L) {
	liServer *srv;
	GHashTable *ht;
	LuaWrapper wrapper;
	const char *key;
	gpointer d;

	srv = (liServer*) lua_touserdata(L, lua_upvalueindex(1));
	lua_pushstring(L, "__ht"); lua_rawget(L, 1);
	ht = (GHashTable*) lua_touserdata(L, -1); lua_pop(L, 1);
	lua_pushstring(L, "__wrapper"); lua_rawget(L, 1);
	wrapper = (LuaWrapper)(intptr_t) lua_touserdata(L, -1); lua_pop(L, 1);
	lua_pushstring(L, "__key"); lua_rawget(L, 1);
	key = luaL_checklstring(L, -1, NULL);

	/* TRACE(srv, "str hash call: '%s'", key); */

	if (key && NULL != (d = g_hash_table_lookup(ht, key))) {
		lua_pop(L, 1);
		return wrapper(srv, L, d);
	}

	lua_pop(L, lua_gettop(L));
	lua_pushstring(L, "lookup failed");
	lua_error(L);
	return 0;
}

#define LUA_PUBLISH_HASH "GHashTable*"
static void lua_push_publish_hash_metatable(liServer *srv, lua_State *L) {
	if (luaL_newmetatable(L, LUA_PUBLISH_HASH)) {
		lua_pushlightuserdata(L, srv);
		lua_pushcclosure(L, lua_str_hash_index, 1);
		lua_setfield(L, -2, "__index");
		
		lua_pushlightuserdata(L, srv);
		lua_pushcclosure(L, lua_str_hash_call, 1);
		lua_setfield(L, -2, "__call");
	}
}

static gboolean publish_str_hash(liServer *srv, lua_State *L, GHashTable *ht, LuaWrapper wrapper) {
	lua_newtable(L);                   /* { } */
	lua_pushlightuserdata(L, ht);
	lua_setfield(L, -2, "__ht");
	lua_pushlightuserdata(L, (void*)(intptr_t)wrapper);
	lua_setfield(L, -2, "__wrapper");

	lua_push_publish_hash_metatable(srv, L);
	lua_setmetatable(L, -2);
	return TRUE;
}


static int handle_server_action(liServer *srv, lua_State *L, gpointer _sa) {
	liServerAction *sa = (liServerAction*) _sa;
	liValue *val;
	liAction *a;

	lua_checkstack(L, 16);
	val = lua_params_to_value(srv, L);

	/* TRACE(srv, "%s", "Creating action"); */
	a = sa->li_create_action(srv, sa->p, val);
	li_value_free(val);

	if (NULL == a) {
		lua_pushstring(L, "creating action failed");
		lua_error(L);
	}

	return lua_push_action(srv, L, a);
}

static int handle_server_setup(liServer *srv, lua_State *L, gpointer _ss) {
	liServerSetup *ss = (liServerSetup*) _ss;
	liValue *val;

	lua_checkstack(L, 16);
	val = lua_params_to_value(srv, L);

	/* TRACE(srv, "%s", "Calling setup"); */

	if (!ss->setup(srv, ss->p, val)) {
		li_value_free(val);
		lua_pushstring(L, "setup failed");
		lua_error(L);
	}

	li_value_free(val);
	return 0;
}

gboolean li_config_lua_load(liServer *srv, const gchar *filename) {
	lua_State *L = srv->L;
	int errfunc;

	li_lua_lock(srv);

	li_lua_new_globals(L);

	if (0 != luaL_loadfile(L, filename)) {
		ERROR(srv, "Loading script '%s' failed: %s", filename, lua_tostring(L, -1));
		return FALSE;
	}

	DEBUG(srv, "Loaded config script '%s'", filename);

	publish_str_hash(srv, L, srv->setups, handle_server_setup);
	lua_setfield(L, LUA_GLOBALSINDEX, "setup");

	publish_str_hash(srv, L, srv->actions, handle_server_action);
	lua_setfield(L, LUA_GLOBALSINDEX, "action");

	lua_push_lvalues_dict(srv, L);

	errfunc = li_lua_push_traceback(L, 0);
	if (lua_pcall(L, 0, 1, errfunc)) {
		ERROR(srv, "lua_pcall(): %s", lua_tostring(L, -1));
		return FALSE;
	}
	lua_remove(L, errfunc);

	lua_pop(L, 1); /* pop the ret-value */

	lua_getfield(L, LUA_GLOBALSINDEX, "actions");
	srv->mainaction = lua_get_action(L, -1);
	li_action_acquire(srv->mainaction);
	lua_pop(L, 1);

	assert(lua_gettop(L) == 0);

	li_lua_restore_globals(L);

	li_lua_unlock(srv);

	return TRUE;
}


#include <lighttpd/core_lua.h>

#include <lualib.h>
#include <lauxlib.h>

static int traceback (lua_State *L) {
	if (!lua_isstring(L, 1))  /* 'message' not a string? */
		return 1;  /* keep it intact */
	lua_getfield(L, LUA_GLOBALSINDEX, "debug");
	if (!lua_istable(L, -1)) {
		lua_pop(L, 1);
		return 1;
	}
	lua_getfield(L, -1, "traceback");
	if (!lua_isfunction(L, -1)) {
		lua_pop(L, 2);
		return 1;
	}
	lua_pushvalue(L, 1);  /* pass error message */
	lua_pushinteger(L, 2);  /* skip this function and traceback */
	lua_call(L, 2, 1);  /* call debug.traceback */
	return 1;
}

int li_lua_push_traceback(lua_State *L, int narg) {
	int base = lua_gettop(L) - narg;  /* function index */
	lua_pushcfunction(L, traceback);
	lua_insert(L, base);
	return base;
}

int li_lua_metatable_index(lua_State *L) {
	/* search for entry in mt, i.e. functions */
	if (!lua_getmetatable(L, 1)) return 0;  /* +1 */

	lua_pushvalue(L, 2);                    /* +1 */
	lua_rawget(L, -2);                      /* */

	if (!lua_isnil(L, -1)) return 1;

	lua_pop(L, 2);                          /* -2 */

	return 0;
}

static void li_lua_store_globals(lua_State *L) {
	/* backup global table reference */
	lua_pushvalue(L, LUA_GLOBALSINDEX); /* +1 */
	lua_setfield(L, LUA_REGISTRYINDEX, "li_globals"); /* -1 */
}

GString* lua_print_get_string(lua_State *L, int from, int to) {
	int i, n = lua_gettop(L);
	GString *buf = g_string_sized_new(0);

	lua_getfield(L, LUA_GLOBALSINDEX, "tostring");
	for (i = from; i <= to; i++) {
		const char *s;
		size_t len;

		lua_pushvalue(L, n+1);
		lua_pushvalue(L, i);
		lua_call(L, 1, 1);
		s = lua_tolstring(L, -1, &len);
		lua_pop(L, 1);

		if (NULL == s) {
			g_string_free(buf, TRUE);
			lua_pushliteral(L, "lua_print_get_string: Couldn't convert parameter to string");
			lua_error(L);
		}
		if (0 == len) continue;
		if (buf->len > 0) {
			g_string_append_c(buf, ' ');
			g_string_append_len(buf, s, len);
		} else {
			g_string_append_len(buf, s, len);
		}
	}
	lua_pop(L, 1);
	return buf;
}

static int li_lua_error(lua_State *L) {
	liServer *srv = lua_touserdata(L, lua_upvalueindex(1));
	GString *buf = lua_print_get_string(L, 1, lua_gettop(L));

	ERROR(srv, "(lua): %s", buf->str);

	g_string_free(buf, TRUE);

	return 0;
}

static int li_lua_warning(lua_State *L) {
	liServer *srv = lua_touserdata(L, lua_upvalueindex(1));
	GString *buf = lua_print_get_string(L, 1, lua_gettop(L));

	WARNING(srv, "(lua): %s", buf->str);

	g_string_free(buf, TRUE);

	return 0;
}

static int li_lua_info(lua_State *L) {
	liServer *srv = lua_touserdata(L, lua_upvalueindex(1));
	GString *buf = lua_print_get_string(L, 1, lua_gettop(L));

	INFO(srv, "(lua): %s", buf->str);

	g_string_free(buf, TRUE);

	return 0;
}

static int li_lua_debug(lua_State *L) {
	liServer *srv = lua_touserdata(L, lua_upvalueindex(1));
	GString *buf = lua_print_get_string(L, 1, lua_gettop(L));

	DEBUG(srv, "(lua): %s", buf->str);

	g_string_free(buf, TRUE);

	return 0;
}

static void lua_push_constants(lua_State *L, int ndx) {
	lua_pushinteger(L, LI_HANDLER_GO_ON);
	lua_setfield(L, ndx, "HANDLER_GO_ON");
	lua_pushinteger(L, LI_HANDLER_COMEBACK);
	lua_setfield(L, ndx, "HANDLER_COMEBACK");
	lua_pushinteger(L, LI_HANDLER_WAIT_FOR_EVENT);
	lua_setfield(L, ndx, "HANDLER_WAIT_FOR_EVENT");
	lua_pushinteger(L, LI_HANDLER_ERROR);
	lua_setfield(L, ndx, "HANDLER_ERROR");
}

void li_lua_init(liServer *srv, lua_State *L) {
	lua_init_chunk_mt(L);
	lua_init_connection_mt(L);
	lua_init_environment_mt(L);
	lua_init_physical_mt(L);
	lua_init_request_mt(L);
	lua_init_response_mt(L);
	lua_init_vrequest_mt(L);

	li_lua_init_stat_mt(L);

	/* prefer closure, but just in case */
	lua_pushlightuserdata(L, srv);
	lua_setfield(L, LUA_REGISTRYINDEX, "lighty.srv");

	lua_newtable(L); /* lighty. */

	lua_pushlightuserdata(L, srv);
	lua_pushcclosure(L, li_lua_error, 1);
		lua_pushvalue(L, -1); /* overwrite global print too */
		lua_setfield(L, LUA_GLOBALSINDEX, "print");
	lua_setfield(L, -2, "print");

	lua_pushlightuserdata(L, srv);
	lua_pushcclosure(L, li_lua_warning, 1);
	lua_setfield(L, -2, "warning");

	lua_pushlightuserdata(L, srv);
	lua_pushcclosure(L, li_lua_info, 1);
	lua_setfield(L, -2, "info");

	lua_pushlightuserdata(L, srv);
	lua_pushcclosure(L, li_lua_debug, 1);
	lua_setfield(L, -2, "debug");

	lua_push_constants(L, -2);

	lua_setfield(L, LUA_GLOBALSINDEX, "lighty");

	li_lua_store_globals(L);
}

void li_lua_restore_globals(lua_State *L) {
	lua_getfield(L, LUA_REGISTRYINDEX, "li_globals"); /* +1 */
	lua_replace(L, LUA_GLOBALSINDEX); /* -1 */
}

void li_lua_new_globals(lua_State *L) {
	lua_newtable(L); /* +1 */

	/* metatable for new global env, link old globals as readonly */
	lua_newtable(L); /* +1 */
	lua_pushvalue(L, LUA_GLOBALSINDEX); /* +1 */
	lua_setfield(L, -2, "__index"); /* -1 */
	lua_setmetatable(L, -2); /* -1 */

	lua_replace(L, LUA_GLOBALSINDEX); /* -1 */
}


static int ghashtable_gstring_next(lua_State *L) {
	GHashTableIter *it = lua_touserdata(L, lua_upvalueindex(1));
	gpointer pkey = NULL, pvalue = NULL;

	/* ignore arguments */
	if (g_hash_table_iter_next(it, &pkey, &pvalue)) {
		GString *key = pkey, *value = pvalue;
		lua_pushlstring(L, key->str, key->len);
		lua_pushlstring(L, value->str, value->len);
		return 2;
	}
	lua_pushnil(L);
	return 1;
}

int li_lua_ghashtable_gstring_pairs(lua_State *L, GHashTable *ht) {
	GHashTableIter *it = lua_newuserdata(L, sizeof(GHashTableIter)); /* +1 */
	g_hash_table_iter_init(it, ht);
	lua_pushcclosure(L, ghashtable_gstring_next, 1);   /* -1, +1 */
	lua_pushnil(L); lua_pushnil(L);                    /* +2 */
	return 3;
}

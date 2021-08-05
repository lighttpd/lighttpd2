
#include <lighttpd/core_lua.h>
#include <lighttpd/actions_lua.h>
#include <lighttpd/condition_lua.h>
#include <lighttpd/value_lua.h>

#include <lualib.h>
#include <lauxlib.h>

liLuaState *li_lua_state_get(lua_State *L) {
	liLuaState *LL;

	lua_getfield(L, LUA_REGISTRYINDEX, LI_LUA_REGISTRY_STATE); /* +1 */
	LL = lua_touserdata(L, -1);
	lua_pop(L, 1);

	LI_FORCE_ASSERT(LL != NULL && LL->L == L);

	return LL;
}

/* replace a negative stack index with a positive one,
 * so that you don't need to care about push/pop
 */
int li_lua_fixindex(lua_State *L, int ndx) {
	int top;
	if (ndx < 0 && ndx >= -(top = lua_gettop(L)))
		ndx = top + ndx + 1;
	return ndx;
}

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

int li_lua_push_traceback(lua_State *L, int nargs) {
	int base = lua_gettop(L) - nargs;  /* function index */
	lua_pushcfunction(L, traceback);
	lua_insert(L, base);
	return base;
}

gboolean li_lua_call_object(liServer *srv, liVRequest *vr, lua_State *L, const char* method, int nargs, int nresults, gboolean optional) {
	int errfunc;
	gboolean result;

	lua_getfield(L, -nargs, method); /* +1 "function" */

	if (!lua_isfunction(L, -1)) {
		if (!optional) {
			_VR_ERROR(srv, vr, "li_lua_call_object: method '%s' not found", method);
		}
		lua_pop(L, 1 + nargs);
		return FALSE;
	}

	lua_insert(L, lua_gettop(L) - nargs); /* move function before arguments */

	errfunc = li_lua_push_traceback(L, nargs); /* +1 "errfunc" */
	if (lua_pcall(L, nargs, nresults, errfunc)) { /* pops func and arguments, push result */
		_VR_ERROR(srv, vr, "lua_pcall(): %s", lua_tostring(L, -1));
		lua_pop(L, 1); /* -1 */
		result = FALSE;
	} else {
		result = TRUE;
	}
	lua_remove(L, errfunc); /* -1 "errfunc" */

	/* "errfunc", "function" and args have been popped - only results remain (and nothing if lua_pcall failed) */
	return result;
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
	lua_setfield(L, LUA_REGISTRYINDEX, LI_LUA_REGISTRY_GLOBALS); /* -1 */
}

GString* li_lua_print_get_string(lua_State *L, int from, int to) {
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
	liWorker *wrk = lua_touserdata(L, lua_upvalueindex(2));
	GString *buf = li_lua_print_get_string(L, 1, lua_gettop(L));

	_ERROR(srv, wrk, NULL, "(lua): %s", buf->str);

	g_string_free(buf, TRUE);

	return 0;
}

static int li_lua_warning(lua_State *L) {
	liServer *srv = lua_touserdata(L, lua_upvalueindex(1));
	liWorker *wrk = lua_touserdata(L, lua_upvalueindex(2));
	GString *buf = li_lua_print_get_string(L, 1, lua_gettop(L));

	_WARNING(srv, wrk, NULL, "(lua): %s", buf->str);

	g_string_free(buf, TRUE);

	return 0;
}

static int li_lua_info(lua_State *L) {
	liServer *srv = lua_touserdata(L, lua_upvalueindex(1));
	liWorker *wrk = lua_touserdata(L, lua_upvalueindex(2));
	GString *buf = li_lua_print_get_string(L, 1, lua_gettop(L));

	_INFO(srv, wrk, NULL, "(lua): %s", buf->str);

	g_string_free(buf, TRUE);

	return 0;
}

static int li_lua_debug(lua_State *L) {
	liServer *srv = lua_touserdata(L, lua_upvalueindex(1));
	liWorker *wrk = lua_touserdata(L, lua_upvalueindex(2));
	GString *buf = li_lua_print_get_string(L, 1, lua_gettop(L));

	_DEBUG(srv, wrk, NULL, "(lua): %s", buf->str);

	g_string_free(buf, TRUE);

	return 0;
}

static int li_lua_md5(lua_State *L) {
	const char *s;
	size_t len;
	char *hash;

	s = lua_tolstring(L, 1, &len);
	if (!s) return 0;

	hash = g_compute_checksum_for_string(G_CHECKSUM_MD5, s, len);
	lua_pushstring(L, hash);
	g_free(hash);

	return 1;
}

static int li_lua_sha1(lua_State *L) {
	const char *s;
	size_t len;
	char *hash;

	s = lua_tolstring(L, 1, &len);
	if (!s) return 0;

	hash = g_compute_checksum_for_string(G_CHECKSUM_SHA1, s, len);
	lua_pushstring(L, hash);
	g_free(hash);

	return 1;
}

static int li_lua_sha256(lua_State *L) {
	const char *s;
	size_t len;
	char *hash;

	s = lua_tolstring(L, 1, &len);
	if (!s) return 0;

	hash = g_compute_checksum_for_string(G_CHECKSUM_SHA256, s, len);
	lua_pushstring(L, hash);
	g_free(hash);

	return 1;
}

static int li_lua_path_simplify(lua_State *L) {
	const char *s;
	size_t len;
	GString *str;

	s = lua_tolstring(L, 1, &len);
	if (!s) return 0;

	str = g_string_new_len(s, len);

	li_path_simplify(str);

	lua_pushlstring(L, GSTR_LEN(str));

	g_string_free(str, TRUE);

	return 1;
}

/* not used right now: using __index metamethod instead to lookup on demand. also not tested yet. */
#if 0
/* should convert a [(key,value)] list to a table t[key] = value;
 * a nil key marks the default entry.
 * returns (t, default_entry)
 * requires unique keys
 */
static int li_lua_kvl_to_table(lua_State *L) {
	int resTable, defValue;
	gboolean haveDefault = FALSE;

	if (!lua_istable(L, 1)) return 0;

	lua_newtable(L);
	resTable = lua_top(L);

	lua_pushnil();
	defValue = lua_top(L);

	lua_pushnil(L); /* start iterating with "no key" */
	while (0 != lua_next(L, 1)) {
		gboolean nil_key = FALSE;
		/* key at -2 and value at -1 */

		/* only lists (numeric keys) with tuples ("tables") as entries */
		if (LUA_TNUMBER != lua_type(L, -2) || !lua_istable(L, -1)) return 0;

		lua_pushnil(L);
		while (0 != lua_next(L, -2)) {
			if (LUA_TNUMBER != lua_type(L, -2)) return 0; /* not a tuple */
			lua_Number n = lua_tonumber(L, -2);
			if (n == 1) { /* key from (k,v) pair */
				switch (lua_type(L, -1)) {
				case LUA_TNIL:
					nil_key = TRUE;
					break;
				case LUA_TSTRING:
					break;
				default:
					return 0; /* wrong key type */
				}
			} else if (n != 2) return 0; /* not a (k,v) pair */
			lua_pop(L, 1);
		}

		if (!nil_key) {
			lua_rawgeti(L, -1, 1);
			lua_gettable(L, resTable);
			if (!lua_isnil(L, -1)) return 0; /* duplicate key */
			lua_pop(L, 1);

			lua_rawgeti(L, -1, 1);
			lua_rawgeti(L, -2, 2);

			lua_settable(L, resTable);
		} else if (haveDefault) {
			return 0; /* duplicate default */
		} else {
			lua_rawgeti(L, -1, 2);
			lua_replace(L, defValue);
		}

		lua_pop(L, 1); /* pop value for next iteration */
	}

	return 2;
}
#endif

static void li_lua_push_lighty_constants(lua_State *L, int ndx) {
	lua_pushinteger(L, LI_HANDLER_GO_ON);
	lua_setfield(L, ndx, "HANDLER_GO_ON");
	lua_pushinteger(L, LI_HANDLER_COMEBACK);
	lua_setfield(L, ndx, "HANDLER_COMEBACK");
	lua_pushinteger(L, LI_HANDLER_WAIT_FOR_EVENT);
	lua_setfield(L, ndx, "HANDLER_WAIT_FOR_EVENT");
	lua_pushinteger(L, LI_HANDLER_ERROR);
	lua_setfield(L, ndx, "HANDLER_ERROR");
}

void li_lua_init2(liLuaState *LL, liServer *srv, liWorker *wrk) {
	lua_State *L = LL->L;

	li_lua_init_chunk_mt(L);
	li_lua_init_environment_mt(L);
	li_lua_init_filter_mt(L);
	li_lua_init_http_headers_mt(L);
	li_lua_init_physical_mt(L);
	li_lua_init_request_mt(L);
	li_lua_init_response_mt(L);
	li_lua_init_stat_mt(L);
	li_lua_init_subrequest_mt(L);
	li_lua_init_virtualrequest_mt(L);

	if (NULL == wrk) {
		/* these should only be used in the "main" lua context */
		li_lua_init_action_mt(srv, L);
		li_lua_init_condition_mt(srv, L);
		li_lua_init_value_mt(L);
	}

	/* prefer closure, but just in case */
	lua_pushlightuserdata(L, srv);
	lua_setfield(L, LUA_REGISTRYINDEX, LI_LUA_REGISTRY_SERVER);
	if (NULL != wrk) {
		lua_pushlightuserdata(L, wrk);
		lua_setfield(L, LUA_REGISTRYINDEX, LI_LUA_REGISTRY_WORKER);
	}

	/* create read-only lighty "table" (zero-sized userdata object) */
	lua_newuserdata(L, 0); /* lighty. */
	lua_newtable(L); /* metatable(lighty) */
	/* prevent tampering with the metatable */
	li_lua_protect_metatable(L);
	lua_newtable(L); /* metatable(lighty).__index */
	/* create lighty.filter_in and lighty.filter_out: */
	li_lua_init_filters(L, srv);

	/* lighty.print (and lighty.error and global print) */
	lua_pushlightuserdata(L, srv);
	lua_pushlightuserdata(L, wrk);
	lua_pushcclosure(L, li_lua_error, 2);
		lua_pushvalue(L, -1); /* overwrite global print too */
		lua_setfield(L, LUA_GLOBALSINDEX, "print");
		lua_pushvalue(L, -1); /* lighty.error alias */
		lua_setfield(L, -3, "error");
	lua_setfield(L, -2, "print");

	/* lighty.warning */
	lua_pushlightuserdata(L, srv);
	lua_pushlightuserdata(L, wrk);
	lua_pushcclosure(L, li_lua_warning, 2);
	lua_setfield(L, -2, "warning");

	/* lighty.info */
	lua_pushlightuserdata(L, srv);
	lua_pushlightuserdata(L, wrk);
	lua_pushcclosure(L, li_lua_info, 2);
	lua_setfield(L, -2, "info");

	/* lighty.debug */
	lua_pushlightuserdata(L, srv);
	lua_pushlightuserdata(L, wrk);
	lua_pushcclosure(L, li_lua_debug, 2);
	lua_setfield(L, -2, "debug");

	/* lighty.{md5,sha1,sha256} */
	lua_pushcclosure(L, li_lua_md5, 0);
	lua_setfield(L, -2, "md5");
	lua_pushcclosure(L, li_lua_sha1, 0);
	lua_setfield(L, -2, "sha1");
	lua_pushcclosure(L, li_lua_sha256, 0);
	lua_setfield(L, -2, "sha256");

	/* lighty.path_simplify */
	lua_pushcclosure(L, li_lua_path_simplify, 0);
	lua_setfield(L, -2, "path_simplify");

	li_lua_push_lighty_constants(L, -2);

	/* set __index for metatable(lighty) */
	lua_setfield(L, -2, "__index");
	/* associate metatable(lighty) */
	lua_setmetatable(L, -2);
	/* store "lighty" object in globals */
	lua_setfield(L, LUA_GLOBALSINDEX, "lighty");

	li_lua_store_globals(L);

	li_plugins_init_lua(LL, srv, wrk);
}

void li_lua_restore_globals(lua_State *L) {
	lua_getfield(L, LUA_REGISTRYINDEX, LI_LUA_REGISTRY_GLOBALS); /* +1 */
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

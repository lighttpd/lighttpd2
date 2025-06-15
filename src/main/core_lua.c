
#include <lighttpd/core_lua.h>
#include <lighttpd/actions_lua.h>
#include <lighttpd/condition_lua.h>
#include <lighttpd/config_lua.h>
#include <lighttpd/value_lua.h>

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

static int traceback(lua_State *L) {
	if (!lua_isstring(L, 1))  /* 'message' not a string? */
		return 1;  /* keep it intact */
	lua_getglobal(L, "debug");
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

	errfunc = li_lua_push_traceback(L, nargs); /* +1 "errfunc", but before func and args */
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

static void li_lua_push_globals(lua_State *L) { /* +1 */
#if LUA_VERSION_NUM == 501
	lua_pushvalue(L, LUA_GLOBALSINDEX); /* +1 */
#else
	lua_rawgeti(L, LUA_REGISTRYINDEX, LUA_RIDX_GLOBALS); /* +1 */
#endif
}

static void li_lua_set_globals(lua_State *L) { /* -1 */
#if LUA_VERSION_NUM == 501
	LI_FORCE_ASSERT(lua_istable(L, -1) && "LUA_GLOBALSINDEX must contain a table in lua5.1");
	lua_replace(L, LUA_GLOBALSINDEX); /* -1 */
#else
	lua_rawseti(L, LUA_REGISTRYINDEX, LUA_RIDX_GLOBALS); /* -1 */
#endif
}

static void create_environment(liLuaState *LL) /* +1 (a new LI_ENV metatable) */ {
	lua_State *L = LL->L;

	lua_createtable(L, 0, 2); /* +1 LI_ENV metatable (return value) */

	lua_newtable(L); /* +1 storage table */

	/* setup storage metatable */
	lua_rawgeti(L, LUA_REGISTRYINDEX, LL->li_env_default_metatable_ref); /* +1 */
	lua_setmetatable(L, -2); /* -1 set default metatable as metatable storage table */

	/* use storage in new ENV metatable */
	lua_pushvalue(L, -1); /* +1, duplicate storage table */
	lua_setfield(L, -3, "__index"); /* -1, __index=storage */
	lua_setfield(L, -2, "__newindex"); /* -1, __newindex=storage */
}

void li_lua_environment_activate_ephemeral(liLuaState *LL) /* +1 (leaves backup of old metatable to restore with) */ {
	lua_State *L = LL->L;

	lua_rawgeti(L, LUA_REGISTRYINDEX, LL->li_env_ref); /* +1 */
	LI_FORCE_ASSERT(lua_getmetatable(L, -1)); /* +1 remember old metatable */
	lua_insert(L, -2); /* swap top two stack elements */

	create_environment(LL); /* +1 */
	lua_setmetatable(L, -2); /* -1 set new environment metatable for LI_ENV */
	lua_pop(L, 1); /* -1 pop LI_ENV */
}

int li_lua_environment_create(liLuaState *LL, liVRequest *vr) /* returns env ref */ {
	lua_State *L = LL->L;
	int *req_ref = NULL;

	create_environment(LL);

	if (NULL != vr) {
		if (LL == &vr->wrk->LL) {
			req_ref = &vr->lua_worker_env_ref;
		} else if (LL == &vr->wrk->srv->LL) {
			req_ref = &vr->lua_server_env_ref;
		}

		if (NULL != req_ref) {
			lua_getfield(L, -1, "__newindex"); /* +1 get storage table */
			if (*req_ref == LUA_NOREF) {
				lua_newtable(L); /* +1 */
				lua_pushvalue(L, -1); /* +1 */
				*req_ref = luaL_ref(L, LUA_REGISTRYINDEX); /* -1 */
			} else {
				lua_rawgeti(L, LUA_REGISTRYINDEX, *req_ref); /* +1 */
			}
			lua_setfield(L, -2, "REQ"); /* -1 set REQ in storage table */
			lua_pop(L, 1); /* -1 pop storage table */
		}
	}

	return luaL_ref(L, LUA_REGISTRYINDEX);
}

void li_lua_environment_activate(liLuaState *LL, int env_mt_ref) /* +1 */ {
	lua_State *L = LL->L;

	lua_rawgeti(L, LUA_REGISTRYINDEX, LL->li_env_ref); /* +1 */
	LI_FORCE_ASSERT(lua_getmetatable(L, -1)); /* +1 remember old metatable */
	lua_insert(L, -2); /* swap top two stack elements */
	lua_rawgeti(L, LUA_REGISTRYINDEX, env_mt_ref); /* +1 */
	lua_setmetatable(L, -2); /* -1 set env_mt for LI_ENV */
	lua_pop(L, 1); /* -1 pop LI_ENV */
}

void li_lua_environment_restore(liLuaState *LL) /* -1 (expects previous metatable to restore on top) */ {
	lua_State *L = LL->L;

	lua_rawgeti(L, LUA_REGISTRYINDEX, LL->li_env_ref); /* +1 */
	lua_pushvalue(L, -2); /* +1 */
	lua_setmetatable(L, -2); /* -1 restore prev mt for LI_ENV */
	lua_pop(L, 2); /* -2 */
}

void li_lua_environment_use_globals(liLuaState *LL) /* +1 */ {
	lua_State *L = LL->L;

	li_lua_push_globals(L); /* +1 backup previous GLOBALS on stack */
	lua_rawgeti(L, LUA_REGISTRYINDEX, LL->li_env_ref); /* +1 */
	li_lua_set_globals(L); /* -1 */
}

void li_lua_environment_restore_globals(lua_State *L) /* -1 */ {
	li_lua_set_globals(L); /* -1 */
}

/* resulting object on top of the stack might eighter be the lua string (owning the returned memory) or an error */
static const char *li_lua_tolstring(lua_State *L, liServer *srv, liVRequest *vr, int idx, size_t *len) { /* +1 */
	int errfunc;

	switch (lua_type(L, idx)) {
	case LUA_TBOOLEAN:
		lua_pushstring(L, (lua_toboolean(L, idx) ? "true" : "false"));
		break;
	case LUA_TNIL:
		lua_pushlstring(L, CONST_STR_LEN("nil"));
		break;
	case LUA_TSTRING:
	case LUA_TNUMBER:
		lua_pushvalue(L, idx);
		break;
	default:
		idx = li_lua_fixindex(L, idx);
		lua_pushcfunction(L, traceback); /* +1 */
		errfunc = lua_gettop(L);
		lua_getglobal(L, "tostring"); /* +1 */
		lua_pushvalue(L, idx); /* +1 object to convert to string */

		if (lua_pcall(L, 1, 1, errfunc)) { /* -2 (func + args), +1 result */
			_VR_ERROR(srv, vr, "li_lua_tolstring failed: %s", lua_tostring(L, -1));

			if (len) *len = 0;
			return NULL;
		}
		lua_remove(L, errfunc); /* -1 */
	}

	return lua_tolstring(L, -1, len);
}

GString* li_lua_print_get_string(lua_State *L, liServer *srv, liVRequest *vr, int from, int to) {
	int i;
	GString *buf = g_string_sized_new(0);

	for (i = from; i <= to; i++) {
		const char *s;
		size_t len;

		if (NULL == (s = li_lua_tolstring(L, srv, vr, i, &len))) { /* +1 */
			s = "<failed tostring>";
			len = 17;
		}

		if (len > 0) {
			if (buf->len > 0) {
				g_string_append_c(buf, ' ');
				li_g_string_append_len(buf, s, len);
			} else {
				li_g_string_append_len(buf, s, len);
			}
		}
		lua_pop(L, 1); /* -1 */
	}
	return buf;
}

static int li_lua_error(lua_State *L) {
	liServer *srv = lua_touserdata(L, lua_upvalueindex(1));
	liWorker *wrk = lua_touserdata(L, lua_upvalueindex(2));
	GString *buf = li_lua_print_get_string(L, srv, NULL, 1, lua_gettop(L));

	_ERROR(srv, wrk, NULL, "(lua): %s", buf->str);

	g_string_free(buf, TRUE);

	return 0;
}

static int li_lua_warning(lua_State *L) {
	liServer *srv = lua_touserdata(L, lua_upvalueindex(1));
	liWorker *wrk = lua_touserdata(L, lua_upvalueindex(2));
	GString *buf = li_lua_print_get_string(L, srv, NULL, 1, lua_gettop(L));

	_WARNING(srv, wrk, NULL, "(lua): %s", buf->str);

	g_string_free(buf, TRUE);

	return 0;
}

static int li_lua_info(lua_State *L) {
	liServer *srv = lua_touserdata(L, lua_upvalueindex(1));
	liWorker *wrk = lua_touserdata(L, lua_upvalueindex(2));
	GString *buf = li_lua_print_get_string(L, srv, NULL, 1, lua_gettop(L));

	_INFO(srv, wrk, NULL, "(lua): %s", buf->str);

	g_string_free(buf, TRUE);

	return 0;
}

static int li_lua_debug(lua_State *L) {
	liServer *srv = lua_touserdata(L, lua_upvalueindex(1));
	liWorker *wrk = lua_touserdata(L, lua_upvalueindex(2));
	GString *buf = li_lua_print_get_string(L, srv, NULL, 1, lua_gettop(L));

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

	/* setup refs (won't need cleanup when we free the lua_State) */
	lua_createtable(L, 0, 1); /* +1 LI_ENV default metatable */
	li_lua_push_globals(L); /* +1 GLOBALS */
	lua_setfield(L, -2, "__index"); /* -1, __index=GLOBALS */
	LL->li_env_default_metatable_ref = luaL_ref(L, LUA_REGISTRYINDEX); /* -1 */

#if LUA_VERSION_NUM == 501
	/* in lua 5.1 the "fenv" / global must be an actual table */
	lua_newtable(L); /* +1 LI_ENV */
	/* as we have a "real" table we prevent new globals through a broken __newindex metatable entry;
	 * build a special metatable for this.
	 */
	lua_createtable(L, 0, 2); /* +1 LI_ENV special metatable */
	li_lua_push_globals(L); /* +1 GLOBALS */
	lua_setfield(L, -2, "__index"); /* -1, __index=GLOBALS */
	lua_newuserdata(L, 0); /* +1 */
	lua_setfield(L, -2, "__newindex"); /* -1, __newindex=EMPTYUSERDATA */
#else
	lua_newuserdata(L, 0); /* +1 LI_ENV */
	lua_rawgeti(L, LUA_REGISTRYINDEX, LL->li_env_default_metatable_ref); /* +1 */
#endif
	lua_setmetatable(L, -2); /* -1 (pops LI_ENV metatable) */
	LL->li_env_ref = luaL_ref(L, LUA_REGISTRYINDEX); /* -1 */

	li_lua_init_chunk_mt(L);
	li_lua_init_environment_mt(L);
	li_lua_init_filter_mt(L);
	li_lua_init_http_headers_mt(L);
	li_lua_init_physical_mt(L);
	li_lua_init_request_mt(L);
	li_lua_init_response_mt(L);
	li_lua_init_stat_mt(L);
	/* DISABLED FOR NOW
	li_lua_init_subrequest_mt(L);
	*/
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
		lua_setglobal(L, "print");
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
	lua_setglobal(L, "lighty");

	li_lua_push_action_table(srv, wrk, L);
	lua_setglobal(L, "action");

	li_lua_set_global_condition_lvalues(srv, L);

	li_plugins_init_lua(LL, srv, wrk);
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


#include <lighttpd/core_lua.h>

#include <lualib.h>
#include <lauxlib.h>

#define LUA_HTTPHEADERS "liHttpHeaders*"

static int lua_http_headers_get(lua_State *L) {
	liHttpHeaders *headers;
	const char *ckey;
	size_t keylen;
	GString *val;

	luaL_checkany(L, 2);
	if (NULL == (headers = li_lua_get_http_headers(L, 1))) return 0;
	if (NULL == (ckey = lua_tolstring(L, 2, &keylen))) return 0;

	val = g_string_sized_new(0);
	li_http_header_get_all(val, headers, ckey, keylen);
	lua_pushlstring(L, val->str, val->len);
	g_string_free(val, TRUE);

	return 1;
}

static int lua_http_headers_index(lua_State *L) {
	if (li_lua_metatable_index(L)) return 1;

	return lua_http_headers_get(L);
}

static int lua_http_headers_set(lua_State *L) {
	liHttpHeaders *headers;
	const char *ckey, *cval;
	size_t keylen, vallen;

	luaL_checkany(L, 3);
	if (NULL == (headers = li_lua_get_http_headers(L, 1))) return 0;
	if (NULL == (ckey = lua_tolstring(L, 2, &keylen))) return 0;
	if (lua_isnil(L, 3)) {
		li_http_header_remove(headers, ckey, keylen);
		return 0;
	}

	if (NULL == (cval = lua_tolstring(L, 3, &vallen))) return 0;

	li_http_header_remove(headers, ckey, keylen);
	li_http_header_insert(headers, ckey, keylen, cval, vallen);
	return 0;
}

static int lua_http_headers_append(lua_State *L) {
	liHttpHeaders *headers;
	const char *ckey, *cval;
	size_t keylen, vallen;

	luaL_checkany(L, 3);
	if (NULL == (headers = li_lua_get_http_headers(L, 1))) return 0;
	if (NULL == (ckey = lua_tolstring(L, 2, &keylen))) return 0;
	if (lua_isnil(L, 3)) {
		return 0;
	}

	if (NULL == (cval = lua_tolstring(L, 3, &vallen))) return 0;

	li_http_header_append(headers, ckey, keylen, cval, vallen);
	return 0;
}

static int lua_http_headers_insert(lua_State *L) {
	liHttpHeaders *headers;
	const char *ckey, *cval;
	size_t keylen, vallen;

	luaL_checkany(L, 3);
	if (NULL == (headers = li_lua_get_http_headers(L, 1))) return 0;
	if (NULL == (ckey = lua_tolstring(L, 2, &keylen))) return 0;
	if (lua_isnil(L, 3)) {
		return 0;
	}

	if (NULL == (cval = lua_tolstring(L, 3, &vallen))) return 0;

	li_http_header_insert(headers, ckey, keylen, cval, vallen);
	return 0;
}

static int lua_http_headers_unset(lua_State *L) {
	liHttpHeaders *headers;
	const char *ckey;
	size_t keylen;

	luaL_checkany(L, 2);
	if (NULL == (headers = li_lua_get_http_headers(L, 1))) return 0;
	if (NULL == (ckey = lua_tolstring(L, 2, &keylen))) return 0;
	li_http_header_remove(headers, ckey, keylen);
	return 0;
}

static int lua_http_headers_clear(lua_State *L) {
	liHttpHeaders *headers;

	luaL_checkany(L, 1);
	if (NULL == (headers = li_lua_get_http_headers(L, 1))) return 0;
	li_http_headers_reset(headers);
	return 0;
}

static int lua_http_headers_next(lua_State *L) {
	liHttpHeader *h;
	liHttpHeaders *headers = lua_touserdata(L, lua_upvalueindex(1));
	GList *l = lua_touserdata(L, lua_upvalueindex(2));
	const char *ckey;
	size_t keylen;
	ckey = lua_tolstring(L, lua_upvalueindex(3), &keylen);

	if (!headers && !l) goto endoflist;

	if (headers) {
		lua_pushnil(L);
		lua_replace(L, lua_upvalueindex(1));
		if (ckey) {
			l = li_http_header_find_first(headers, ckey, keylen);
		} else {
			l = headers->entries.head;
		}
	} else {
		if (ckey) {
			l = li_http_header_find_next(l, ckey, keylen);
		} else {
			l = g_list_next(l);
		}
	}

	if (!l) goto endoflist;

	h = l->data;
	lua_pushlstring(L, h->data->str, h->keylen);
	if (h->data->len > h->keylen + 2) {
		lua_pushlstring(L, h->data->str + (h->keylen + 2), h->data->len - (h->keylen + 2));
	} else {
		lua_pushliteral(L, "");
	}

	lua_pushlightuserdata(L, l);
	lua_replace(L, lua_upvalueindex(2));

	return 2;

endoflist:
	lua_pushnil(L);
	lua_replace(L, lua_upvalueindex(1));
	lua_pushnil(L);
	lua_replace(L, lua_upvalueindex(2));
	lua_pushnil(L);
	lua_replace(L, lua_upvalueindex(3));
	lua_pushnil(L);
	return 1;
}

static int lua_http_headers_pairs(lua_State *L) {
	liHttpHeaders *headers;
	gboolean haskey = FALSE;

	luaL_checkany(L, 1);
	headers = li_lua_get_http_headers(L, 1);

	if (lua_gettop(L) == 2) {
		luaL_checkstring(L, 2);
		haskey = TRUE;
	}

	if (!headers) return 0;

	lua_pushlightuserdata(L, headers);
	lua_pushlightuserdata(L, NULL);
	if (haskey) {
		lua_pushvalue(L, 2);
	} else {
		lua_pushnil(L);
	}
	lua_pushcclosure(L, lua_http_headers_next, 3);

	return 1;
}

static const luaL_Reg http_headers_mt[] = {
	{ "__index", lua_http_headers_index },
	{ "get", lua_http_headers_get },
	{ "__newindex", lua_http_headers_set },
	{ "set", lua_http_headers_set },
	{ "append", lua_http_headers_append },
	{ "insert", lua_http_headers_insert },
	{ "unset", lua_http_headers_unset },
	{ "__pairs", lua_http_headers_pairs },
	{ "pairs", lua_http_headers_pairs },
	{ "list", lua_http_headers_pairs },
	{ "clear", lua_http_headers_clear },

	{ NULL, NULL }
};

static void init_http_headers_mt(lua_State *L) {
	luaL_register(L, NULL, http_headers_mt);
}

void li_lua_init_http_headers_mt(lua_State *L) {
	if (luaL_newmetatable(L, LUA_HTTPHEADERS)) {
		init_http_headers_mt(L);
	}
	lua_pop(L, 1);
}

liHttpHeaders* li_lua_get_http_headers(lua_State *L, int ndx) {
	if (!lua_isuserdata(L, ndx)) return NULL;
	if (!lua_getmetatable(L, ndx)) return NULL;
	luaL_getmetatable(L, LUA_HTTPHEADERS);
	if (lua_isnil(L, -1) || lua_isnil(L, -2) || !lua_equal(L, -1, -2)) {
		lua_pop(L, 2);
		return NULL;
	}
	lua_pop(L, 2);
	return *(liHttpHeaders**) lua_touserdata(L, ndx);
}

int li_lua_push_http_headers(lua_State *L, liHttpHeaders *headers) {
	liHttpHeaders **pheaders;

	if (NULL == headers) {
		lua_pushnil(L);
		return 1;
	}

	pheaders = (liHttpHeaders**) lua_newuserdata(L, sizeof(liHttpHeaders*));
	*pheaders = headers;

	if (luaL_newmetatable(L, LUA_HTTPHEADERS)) {
		init_http_headers_mt(L);
	}

	lua_setmetatable(L, -2);
	return 1;
}

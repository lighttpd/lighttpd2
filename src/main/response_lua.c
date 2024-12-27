
#include <lighttpd/core_lua.h>

#define LUA_RESPONSE "liResponse*"

typedef int (*lua_Response_Attrib)(liResponse *resp, lua_State *L);

static int lua_response_attr_read_headers(liResponse *resp, lua_State *L) {
	li_lua_push_http_headers(L, resp->headers);
	return 1;
}

static int lua_response_attr_read_status(liResponse *resp, lua_State *L) {
	lua_pushinteger(L, resp->http_status);
	return 1;
}

static int lua_response_attr_write_status(liResponse *resp, lua_State *L) {
	int status = (int) luaL_checkinteger(L, 3);
	if (status < 200 || status > 999) {
		lua_pushliteral(L, "Invalid http response status: ");
		lua_pushinteger(L, status);
		lua_concat(L, 2);
		lua_error(L);
	}
	resp->http_status = status;
	return 0;
}


#define AR(m) { #m, lua_response_attr_read_##m, NULL }
#define AW(m) { #m, NULL, lua_response_attr_write_##m }
#define ARW(m) { #m, lua_response_attr_read_##m, lua_response_attr_write_##m }

static const struct {
	const char* key;
	lua_Response_Attrib read_attr, write_attr;
} response_attribs[] = {
	AR(headers),
	ARW(status),

	{ NULL, NULL, NULL }
};

static int lua_response_index(lua_State *L) {
	liResponse *resp;
	const char *key;
	int i;

	if (lua_gettop(L) != 2) {
		lua_pushstring(L, "incorrect number of arguments");
		lua_error(L);
	}

	if (li_lua_metatable_index(L)) return 1;

	resp = li_lua_get_response(L, 1);
	if (!resp) return 0;

	if (lua_isnumber(L, 2)) return 0;
	if (!lua_isstring(L, 2)) return 0;

	key = lua_tostring(L, 2);
	for (i = 0; response_attribs[i].key ; i++) {
		if (0 == strcmp(key, response_attribs[i].key)) {
			if (response_attribs[i].read_attr)
				return response_attribs[i].read_attr(resp, L);
			break;
		}
	}

	lua_pushstring(L, "cannot read attribute ");
	lua_pushstring(L, key);
	lua_pushstring(L, " in response");
	lua_concat(L, 3);
	lua_error(L);

	return 0;
}

static int lua_response_newindex(lua_State *L) {
	liResponse *resp;
	const char *key;
	int i;

	if (lua_gettop(L) != 3) {
		lua_pushstring(L, "incorrect number of arguments");
		lua_error(L);
	}

	resp = li_lua_get_response(L, 1);
	if (!resp) return 0;

	if (lua_isnumber(L, 2)) return 0;
	if (!lua_isstring(L, 2)) return 0;

	key = lua_tostring(L, 2);
	for (i = 0; response_attribs[i].key ; i++) {
		if (0 == strcmp(key, response_attribs[i].key)) {
			if (response_attribs[i].write_attr)
				return response_attribs[i].write_attr(resp, L);
			break;
		}
	}

	lua_pushstring(L, "cannot write attribute ");
	lua_pushstring(L, key);
	lua_pushstring(L, "in response");
	lua_concat(L, 3);
	lua_error(L);

	return 0;
}

static const luaL_Reg response_mt[] = {
	{ "__index", lua_response_index },
	{ "__newindex", lua_response_newindex },

	{ NULL, NULL }
};

static HEDLEY_NEVER_INLINE void init_response_mt(lua_State *L) {
	li_lua_setfuncs(L, response_mt);
}

static void lua_push_response_metatable(lua_State *L) {
	if (li_lua_new_protected_metatable(L, LUA_RESPONSE)) {
		init_response_mt(L);
	}
}

void li_lua_init_response_mt(lua_State *L) {
	lua_push_response_metatable(L);
	lua_pop(L, 1);
}

liResponse* li_lua_get_response(lua_State *L, int ndx) {
	if (!lua_isuserdata(L, ndx)) return NULL;
	if (!lua_getmetatable(L, ndx)) return NULL;
	luaL_getmetatable(L, LUA_RESPONSE);
	if (lua_isnil(L, -1) || lua_isnil(L, -2) || !li_lua_equal(L, -1, -2)) {
		lua_pop(L, 2);
		return NULL;
	}
	lua_pop(L, 2);
	return *(liResponse**) lua_touserdata(L, ndx);
}

int li_lua_push_response(lua_State *L, liResponse *resp) {
	liResponse **presp;

	if (NULL == resp) {
		lua_pushnil(L);
		return 1;
	}

	presp = (liResponse**) lua_newuserdata(L, sizeof(liResponse*));
	*presp = resp;

	lua_push_response_metatable(L);
	lua_setmetatable(L, -2);
	return 1;
}

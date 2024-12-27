
#include <lighttpd/core_lua.h>

#define LUA_REQUEST "liRequest*"
#define LUA_REQUESTURI "liRequestUri*"

typedef int (*lua_Request_Attrib)(liRequest *req, lua_State *L);

static int lua_request_attr_read_headers(liRequest *req, lua_State *L) {
	li_lua_push_http_headers(L, req->headers);
	return 1;
}

static int lua_request_attr_read_http_method(liRequest *req, lua_State *L) {
	lua_pushlstring(L, req->http_method_str->str, req->http_method_str->len);
	return 1;
}

static int lua_request_attr_read_http_version(liRequest *req, lua_State *L) {
	switch (req->http_version) {
	case LI_HTTP_VERSION_1_0:
		lua_pushliteral(L, "HTTP/1.0");
		break;
	case LI_HTTP_VERSION_1_1:
		lua_pushliteral(L, "HTTP/1.1");
		break;
	case LI_HTTP_VERSION_UNSET:
	default:
		lua_pushnil(L);
	}
	return 1;
}

static int lua_request_attr_read_content_length(liRequest *req, lua_State *L) {
	lua_pushinteger(L, req->content_length);
	return 1;
}

static int lua_request_attr_read_uri(liRequest *req, lua_State *L) {
	li_lua_push_requesturi(L, &req->uri);
	return 1;
}

#define AR(m) { #m, lua_request_attr_read_##m, NULL }
#define AW(m) { #m, NULL, lua_request_attr_write_##m }
#define ARW(m) { #m, lua_request_attr_read_##m, lua_request_attr_write_##m }

static const struct {
	const char* key;
	lua_Request_Attrib read_attr, write_attr;
} request_attribs[] = {
	AR(headers),
	AR(http_method),
	AR(http_version),
	AR(content_length),
	AR(uri),

	{ NULL, NULL, NULL }
};

#undef AR
#undef AW
#undef ARW


static int lua_request_index(lua_State *L) {
	liRequest *req;
	const char *key;
	int i;

	if (lua_gettop(L) != 2) {
		lua_pushstring(L, "incorrect number of arguments");
		lua_error(L);
	}

	if (li_lua_metatable_index(L)) return 1;

	req = li_lua_get_request(L, 1);
	if (!req) return 0;

	if (lua_isnumber(L, 2)) return 0;
	if (!lua_isstring(L, 2)) return 0;

	key = lua_tostring(L, 2);
	for (i = 0; request_attribs[i].key ; i++) {
		if (0 == strcmp(key, request_attribs[i].key)) {
			if (request_attribs[i].read_attr)
				return request_attribs[i].read_attr(req, L);
			break;
		}
	}

	lua_pushstring(L, "cannot read attribute ");
	lua_pushstring(L, key);
	lua_pushstring(L, " in request");
	lua_concat(L, 3);
	lua_error(L);

	return 0;
}

static int lua_request_newindex(lua_State *L) {
	liRequest *req;
	const char *key;
	int i;

	if (lua_gettop(L) != 3) {
		lua_pushstring(L, "incorrect number of arguments");
		lua_error(L);
	}

	req = li_lua_get_request(L, 1);
	if (!req) return 0;

	if (lua_isnumber(L, 2)) return 0;
	if (!lua_isstring(L, 2)) return 0;

	key = lua_tostring(L, 2);
	for (i = 0; request_attribs[i].key ; i++) {
		if (0 == strcmp(key, request_attribs[i].key)) {
			if (request_attribs[i].write_attr)
				return request_attribs[i].write_attr(req, L);
			break;
		}
	}

	lua_pushstring(L, "cannot write attribute ");
	lua_pushstring(L, key);
	lua_pushstring(L, "in request");
	lua_concat(L, 3);
	lua_error(L);

	return 0;
}

static const luaL_Reg request_mt[] = {
	{ "__index", lua_request_index },
	{ "__newindex", lua_request_newindex },

	{ NULL, NULL }
};

static HEDLEY_NEVER_INLINE void init_request_mt(lua_State *L) {
	li_lua_setfuncs(L, request_mt);
}

static void lua_push_request_metatable(lua_State *L) {
	if (li_lua_new_protected_metatable(L, LUA_REQUEST)) {
		init_request_mt(L);
	}
}

typedef int (*lua_RequestUri_Attrib)(liRequestUri *uri, lua_State *L);

#define DEF_LUA_MODIFY_GSTRING(attr)                                           \
static int lua_requesturi_attr_read_##attr(liRequestUri *uri, lua_State *L) {  \
	lua_pushlstring(L, uri->attr->str, uri->attr->len);                        \
	return 1;                                                                  \
}                                                                              \
                                                                               \
static int lua_requesturi_attr_write_##attr(liRequestUri *uri, lua_State *L) { \
	const char *s; size_t len;                                                 \
	luaL_checkstring(L, 3);                                                    \
	s = lua_tolstring(L, 3, &len);                                             \
	g_string_truncate(uri->attr, 0);                                           \
	li_g_string_append_len(uri->attr, s, len);                                    \
	return 0;                                                                  \
}

DEF_LUA_MODIFY_GSTRING(raw)
DEF_LUA_MODIFY_GSTRING(raw_path)
DEF_LUA_MODIFY_GSTRING(raw_orig_path)
DEF_LUA_MODIFY_GSTRING(scheme)
DEF_LUA_MODIFY_GSTRING(authority)
DEF_LUA_MODIFY_GSTRING(path)
DEF_LUA_MODIFY_GSTRING(query)
DEF_LUA_MODIFY_GSTRING(host)

#undef DEF_LUA_MODIFY_GSTRING

#define AR(m) { #m, lua_requesturi_attr_read_##m, NULL }
#define AW(m) { #m, NULL, lua_requesturi_attr_write_##m }
#define ARW(m) { #m, lua_requesturi_attr_read_##m, lua_requesturi_attr_write_##m }

static const struct {
	const char* key;
	lua_RequestUri_Attrib read_attr, write_attr;
} requesturi_attribs[] = {
	ARW(raw),
	ARW(raw_path),
	ARW(raw_orig_path),
	ARW(scheme),
	ARW(authority),
	ARW(path),
	ARW(query),
	ARW(host),

	{ NULL, NULL, NULL }
};

#undef AR
#undef AW
#undef ARW


static int lua_requesturi_index(lua_State *L) {
	liRequestUri *uri;
	const char *key;
	int i;

	if (lua_gettop(L) != 2) {
		lua_pushstring(L, "incorrect number of arguments");
		lua_error(L);
	}

	if (li_lua_metatable_index(L)) return 1;

	uri = li_lua_get_requesturi(L, 1);
	if (!uri) return 0;

	if (lua_isnumber(L, 2)) return 0;
	if (!lua_isstring(L, 2)) return 0;

	key = lua_tostring(L, 2);
	for (i = 0; requesturi_attribs[i].key ; i++) {
		if (0 == strcmp(key, requesturi_attribs[i].key)) {
			if (requesturi_attribs[i].read_attr)
				return requesturi_attribs[i].read_attr(uri, L);
			break;
		}
	}

	lua_pushstring(L, "cannot read attribute ");
	lua_pushstring(L, key);
	lua_pushstring(L, " in request uri");
	lua_concat(L, 3);
	lua_error(L);

	return 0;
}

static int lua_requesturi_newindex(lua_State *L) {
	liRequestUri *uri;
	const char *key;
	int i;

	if (lua_gettop(L) != 3) {
		lua_pushstring(L, "incorrect number of arguments");
		lua_error(L);
	}

	uri = li_lua_get_requesturi(L, 1);
	if (!uri) return 0;

	if (lua_isnumber(L, 2)) return 0;
	if (!lua_isstring(L, 2)) return 0;

	key = lua_tostring(L, 2);
	for (i = 0; requesturi_attribs[i].key ; i++) {
		if (0 == strcmp(key, requesturi_attribs[i].key)) {
			if (requesturi_attribs[i].write_attr)
				return requesturi_attribs[i].write_attr(uri, L);
			break;
		}
	}

	lua_pushstring(L, "cannot write attribute ");
	lua_pushstring(L, key);
	lua_pushstring(L, "in request uri");
	lua_concat(L, 3);
	lua_error(L);

	return 0;
}


static const luaL_Reg requesturi_mt[] = {
	{ "__index", lua_requesturi_index },
	{ "__newindex", lua_requesturi_newindex },

	{ NULL, NULL }
};

static HEDLEY_NEVER_INLINE void init_requesturi_mt(lua_State *L) {
	li_lua_setfuncs(L, requesturi_mt);
}

static void lua_push_requesturi_metatable(lua_State *L) {
	if (li_lua_new_protected_metatable(L, LUA_REQUESTURI)) {
		init_requesturi_mt(L);
	}
}

void li_lua_init_request_mt(lua_State *L) {
	lua_push_request_metatable(L);
	lua_pop(L, 1);

	lua_push_requesturi_metatable(L);
	lua_pop(L, 1);
}

liRequest* li_lua_get_request(lua_State *L, int ndx) {
	if (!lua_isuserdata(L, ndx)) return NULL;
	if (!lua_getmetatable(L, ndx)) return NULL;
	luaL_getmetatable(L, LUA_REQUEST);
	if (lua_isnil(L, -1) || lua_isnil(L, -2) || !li_lua_equal(L, -1, -2)) {
		lua_pop(L, 2);
		return NULL;
	}
	lua_pop(L, 2);
	return *(liRequest**) lua_touserdata(L, ndx);
}

int li_lua_push_request(lua_State *L, liRequest *req) {
	liRequest **preq;

	if (NULL == req) {
		lua_pushnil(L);
		return 1;
	}

	preq = (liRequest**) lua_newuserdata(L, sizeof(liRequest*));
	*preq = req;

	lua_push_request_metatable(L);
	lua_setmetatable(L, -2);
	return 1;
}

liRequestUri* li_lua_get_requesturi(lua_State *L, int ndx) {
	if (!lua_isuserdata(L, ndx)) return NULL;
	if (!lua_getmetatable(L, ndx)) return NULL;
	luaL_getmetatable(L, LUA_REQUESTURI);
	if (lua_isnil(L, -1) || lua_isnil(L, -2) || !li_lua_equal(L, -1, -2)) {
		lua_pop(L, 2);
		return NULL;
	}
	lua_pop(L, 2);
	return *(liRequestUri**) lua_touserdata(L, ndx);
}

int li_lua_push_requesturi(lua_State *L, liRequestUri *uri) {
	liRequestUri **puri;

	if (NULL == uri) {
		lua_pushnil(L);
		return 1;
	}

	puri = (liRequestUri**) lua_newuserdata(L, sizeof(liRequestUri*));
	*puri = uri;

	lua_push_requesturi_metatable(L);
	lua_setmetatable(L, -2);
	return 1;
}


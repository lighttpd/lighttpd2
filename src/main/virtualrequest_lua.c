
#include <lighttpd/core_lua.h>

#include <lualib.h>
#include <lauxlib.h>

#define LUA_VREQUEST "liVRequest*"

typedef int (*lua_VRequest_Attrib)(liVRequest *vr, lua_State *L);

static int lua_vrequest_attr_read_in(liVRequest *vr, lua_State *L) {
	lua_push_chunkqueue(L, vr->in);
	return 1;
}

static int lua_vrequest_attr_read_out(liVRequest *vr, lua_State *L) {
	lua_push_chunkqueue(L, vr->out);
	return 1;
}

static int lua_vrequest_attr_read_con(liVRequest *vr, lua_State *L) {
	lua_push_connection(L, vr->con);
	return 1;
}

static int lua_vrequest_attr_read_env(liVRequest *vr, lua_State *L) {
	lua_push_environment(L, &vr->env);
	return 1;
}

static int lua_vrequest_attr_read_req(liVRequest *vr, lua_State *L) {
	lua_push_request(L, &vr->request);
	return 1;
}

static int lua_vrequest_attr_read_resp(liVRequest *vr, lua_State *L) {
	lua_push_response(L, &vr->response);
	return 1;
}

static int lua_vrequest_attr_read_phys(liVRequest *vr, lua_State *L) {
	lua_push_physical(L, &vr->physical);
	return 1;
}

#define AR(m) { #m, lua_vrequest_attr_read_##m, NULL }
#define AW(m) { #m, NULL, lua_vrequest_attr_write_##m }
#define ARW(m) { #m, lua_vrequest_attr_read_##m, lua_vrequest_attr_write_##m }

static const struct {
	const char* key;
	lua_VRequest_Attrib read_attr, write_attr;
} vrequest_attribs[] = {
	AR(con),
	AR(in),
	AR(out),
	AR(env),
	AR(req),
	AR(resp),
	AR(phys),

	{ NULL, NULL, NULL }
};

static int lua_vrequest_index(lua_State *L) {
	liVRequest *vr;
	const char *key;
	int i;

	if (lua_gettop(L) != 2) {
		lua_pushstring(L, "incorrect number of arguments");
		lua_error(L);
	}

	if (li_lua_metatable_index(L)) return 1;

	vr = lua_get_vrequest(L, 1);
	if (!vr) return 0;

	if (lua_isnumber(L, 2)) return 0;
	if (!lua_isstring(L, 2)) return 0;

	key = lua_tostring(L, 2);
	for (i = 0; vrequest_attribs[i].key ; i++) {
		if (0 == strcmp(key, vrequest_attribs[i].key)) {
			if (vrequest_attribs[i].read_attr)
				return vrequest_attribs[i].read_attr(vr, L);
			break;
		}
	}

	lua_pushstring(L, "cannot read attribute ");
	lua_pushstring(L, key);
	lua_pushstring(L, " in vrequest");
	lua_concat(L, 3);
	lua_error(L);

	return 0;
}

static int lua_vrequest_newindex(lua_State *L) {
	liVRequest *vr;
	const char *key;
	int i;

	if (lua_gettop(L) != 3) {
		lua_pushstring(L, "incorrect number of arguments");
		lua_error(L);
	}

	vr = lua_get_vrequest(L, 1);
	if (!vr) return 0;

	if (lua_isnumber(L, 2)) return 0;
	if (!lua_isstring(L, 2)) return 0;

	key = lua_tostring(L, 2);
	for (i = 0; vrequest_attribs[i].key ; i++) {
		if (0 == strcmp(key, vrequest_attribs[i].key)) {
			if (vrequest_attribs[i].write_attr)
				return vrequest_attribs[i].write_attr(vr, L);
			break;
		}
	}

	lua_pushstring(L, "cannot write attribute ");
	lua_pushstring(L, key);
	lua_pushstring(L, "in vrequest");
	lua_concat(L, 3);
	lua_error(L);

	return 0;
}

static int lua_vrequest_error(lua_State *L) {
	liVRequest *vr;
	GString *buf;
	vr = lua_get_vrequest(L, 1);

	buf = lua_print_get_string(L, 2, lua_gettop(L));

	VR_ERROR(vr, "(lua): %s", buf->str);

	g_string_free(buf, TRUE);

	return 0;
}

static int lua_vrequest_warning(lua_State *L) {
	liVRequest *vr;
	GString *buf;
	vr = lua_get_vrequest(L, 1);

	buf = lua_print_get_string(L, 2, lua_gettop(L));

	VR_WARNING(vr, "(lua): %s", buf->str);

	g_string_free(buf, TRUE);

	return 0;
}

static int lua_vrequest_info(lua_State *L) {
	liVRequest *vr;
	GString *buf;
	vr = lua_get_vrequest(L, 1);

	buf = lua_print_get_string(L, 2, lua_gettop(L));

	VR_INFO(vr, "(lua): %s", buf->str);

	g_string_free(buf, TRUE);

	return 0;
}

static int lua_vrequest_debug(lua_State *L) {
	liVRequest *vr;
	GString *buf;
	vr = lua_get_vrequest(L, 1);

	buf = lua_print_get_string(L, 2, lua_gettop(L));

	VR_DEBUG(vr, "(lua): %s", buf->str);

	g_string_free(buf, TRUE);

	return 0;
}

static int lua_vrequest_handle_direct(lua_State *L) {
	liVRequest *vr;
	vr = lua_get_vrequest(L, 1);

	lua_pushboolean(L, li_vrequest_handle_direct(vr));

	return 1;
}

static int lua_vrequest_is_handled(lua_State *L) {
	liVRequest *vr;
	vr = lua_get_vrequest(L, 1);

	lua_pushboolean(L, li_vrequest_is_handled(vr));

	return 1;
}

static const luaL_Reg vrequest_mt[] = {
	{ "__index", lua_vrequest_index },
	{ "__newindex", lua_vrequest_newindex },

	{ "print", lua_vrequest_error },
	{ "warning", lua_vrequest_warning },
	{ "info", lua_vrequest_info },
	{ "debug", lua_vrequest_debug },

	{ "handle_direct", lua_vrequest_handle_direct },
	{ "is_handled", lua_vrequest_is_handled },

	{ NULL, NULL }
};

static void init_vrequest_mt(lua_State *L) {
	luaL_register(L, NULL, vrequest_mt);
}

void lua_init_vrequest_mt(lua_State *L) {
	if (luaL_newmetatable(L, LUA_VREQUEST)) {
		init_vrequest_mt(L);
	}
	lua_pop(L, 1);
}

liVRequest* lua_get_vrequest(lua_State *L, int ndx) {
	if (!lua_isuserdata(L, ndx)) return NULL;
	if (!lua_getmetatable(L, ndx)) return NULL;
	luaL_getmetatable(L, LUA_VREQUEST);
	if (lua_isnil(L, -1) || lua_isnil(L, -2) || !lua_equal(L, -1, -2)) {
		lua_pop(L, 2);
		return NULL;
	}
	lua_pop(L, 2);
	return *(liVRequest**) lua_touserdata(L, ndx);
}

int lua_push_vrequest(lua_State *L, liVRequest *vr) {
	liVRequest **pvr;

	pvr = (liVRequest**) lua_newuserdata(L, sizeof(liVRequest*));
	*pvr = vr;

	if (luaL_newmetatable(L, LUA_VREQUEST)) {
		init_vrequest_mt(L);
	}

	lua_setmetatable(L, -2);
	return 1;
}


#include <lighttpd/core_lua.h>
#include <lighttpd/actions_lua.h>

#define LUA_VREQUEST "liVRequest*"

typedef int (*lua_VRequest_Attrib)(liVRequest *vr, lua_State *L);

static int lua_vrequest_attr_read_in(liVRequest *vr, lua_State *L) {
	li_lua_push_chunkqueue(L, (NULL != vr->backend_drain) ? vr->backend_drain->out : NULL);
	return 1;
}

static int lua_vrequest_attr_read_out(liVRequest *vr, lua_State *L) {
	li_lua_push_chunkqueue(L, (NULL != vr->backend_source) ? vr->backend_source->out : NULL);
	return 1;
}

static int lua_vrequest_attr_read_con(liVRequest *vr, lua_State *L) {
	li_lua_push_coninfo(L, vr->coninfo);
	return 1;
}

static int lua_vrequest_attr_read_env(liVRequest *vr, lua_State *L) {
	li_lua_push_environment(L, &vr->env);
	return 1;
}

static int lua_vrequest_attr_read_req(liVRequest *vr, lua_State *L) {
	li_lua_push_request(L, &vr->request);
	return 1;
}

static int lua_vrequest_attr_read_resp(liVRequest *vr, lua_State *L) {
	li_lua_push_response(L, &vr->response);
	return 1;
}

static int lua_vrequest_attr_read_phys(liVRequest *vr, lua_State *L) {
	li_lua_push_physical(L, &vr->physical);
	return 1;
}

static int lua_vrequest_attr_read_is_handled(liVRequest *vr, lua_State *L) {
	lua_pushboolean(L, li_vrequest_is_handled(vr));
	return 1;
}

static int lua_vrequest_attr_read_has_response(liVRequest *vr, lua_State *L) {
	lua_pushboolean(L, vr->state >= LI_VRS_HANDLE_RESPONSE_HEADERS);
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
	AR(is_handled),
	AR(has_response),

	{ NULL, NULL, NULL }
};

#undef AR
#undef AW
#undef ARW

static int lua_vrequest_index(lua_State *L) {
	liVRequest *vr;
	const char *key;
	int i;

	if (lua_gettop(L) != 2) {
		lua_pushstring(L, "incorrect number of arguments");
		lua_error(L);
	}

	if (li_lua_metatable_index(L)) return 1;

	vr = li_lua_get_vrequest(L, 1);
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

	vr = li_lua_get_vrequest(L, 1);
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
	vr = li_lua_get_vrequest(L, 1);

	buf = li_lua_print_get_string(L, 2, lua_gettop(L));

	VR_ERROR(vr, "(lua): %s", buf->str);

	g_string_free(buf, TRUE);

	return 0;
}

static int lua_vrequest_warning(lua_State *L) {
	liVRequest *vr;
	GString *buf;
	vr = li_lua_get_vrequest(L, 1);

	buf = li_lua_print_get_string(L, 2, lua_gettop(L));

	VR_WARNING(vr, "(lua): %s", buf->str);

	g_string_free(buf, TRUE);

	return 0;
}

static int lua_vrequest_info(lua_State *L) {
	liVRequest *vr;
	GString *buf;
	vr = li_lua_get_vrequest(L, 1);

	buf = li_lua_print_get_string(L, 2, lua_gettop(L));

	VR_INFO(vr, "(lua): %s", buf->str);

	g_string_free(buf, TRUE);

	return 0;
}

static int lua_vrequest_debug(lua_State *L) {
	liVRequest *vr;
	GString *buf;
	vr = li_lua_get_vrequest(L, 1);

	buf = li_lua_print_get_string(L, 2, lua_gettop(L));

	VR_DEBUG(vr, "(lua): %s", buf->str);

	g_string_free(buf, TRUE);

	return 0;
}

/* st, res, errno, msg = vr:stat(filename)
 *  st: stat data (nil if not available (yet))
 *  res: error code (HANDLE_GO_ON if successful)
 *  errno: errno returned by stat() (only for HANDLER_ERROR)
 *  msg: error message for errno
 */
static int lua_vrequest_stat(lua_State *L) {
	liVRequest *vr;
	GString path;
	const char *filename;
	size_t filename_len;
	liHandlerResult res;
	int err = 0;
	struct stat st;

	if (lua_gettop(L) != 2) {
		lua_pushstring(L, "vr:stat(filename): incorrect number of arguments");
		lua_error(L);
	}

	vr = li_lua_get_vrequest(L, 1);
	if (!vr || !lua_isstring(L, 2)) {
		lua_pushstring(L, "vr:stat(filename): wrong argument types");
		lua_error(L);
	}

	filename = lua_tolstring(L, 2, &filename_len);
	path = li_const_gstring(filename, filename_len);

	res = li_stat_cache_get(vr, &path, &st, &err, NULL);
	switch (res) {
	case LI_HANDLER_GO_ON:
		li_lua_push_stat(L, &st);
		lua_pushinteger(L, res);
		return 2;
	case LI_HANDLER_WAIT_FOR_EVENT:
		lua_pushnil(L);
		lua_pushinteger(L, res);
		return 2;
	case LI_HANDLER_ERROR:
		lua_pushnil(L);
		lua_pushinteger(L, res);
		lua_pushinteger(L, err);
		lua_pushstring(L, g_strerror(err));
		return 4;
	case LI_HANDLER_COMEBACK:
		VR_ERROR(vr, "%s", "Unexpected return value from li_stat_cache_get: LI_HANDLER_COMEBACK");
		lua_pushnil(L);
		lua_pushinteger(L, LI_HANDLER_ERROR);
		return 2;
	}

	return 0;
}

static int lua_vrequest_handle_direct(lua_State *L) {
	liVRequest *vr;
	vr = li_lua_get_vrequest(L, 1);

	lua_pushboolean(L, li_vrequest_handle_direct(vr));

	return 1;
}

static int lua_vrequest_enter_action(lua_State *L) {
	liVRequest *vr;
	liAction *act;

	if (lua_gettop(L) != 2) {
		lua_pushstring(L, "incorrect number of arguments");
		lua_error(L);
	}

	vr = li_lua_get_vrequest(L, 1);
	act = li_lua_get_action(L, 2);
	if (!vr || !act) {
		lua_pushstring(L, "wrong arguments");
		lua_error(L);
	}

	li_action_enter(vr, act);

	return 0;
}

static int lua_vrequest_add_filter_in(lua_State *L) {
	liVRequest *vr;

	if (lua_gettop(L) != 2) {
		lua_pushstring(L, "incorrect number of arguments");
		lua_error(L);
	}

	vr = li_lua_get_vrequest(L, 1);

	return li_lua_push_filter(L, li_lua_vrequest_add_filter_in(L, vr, 2));
}

static int lua_vrequest_add_filter_out(lua_State *L) {
	liVRequest *vr;

	if (lua_gettop(L) != 2) {
		lua_pushstring(L, "incorrect number of arguments");
		lua_error(L);
	}

	vr = li_lua_get_vrequest(L, 1);

	return li_lua_push_filter(L, li_lua_vrequest_add_filter_out(L, vr, 2));
}

static const luaL_Reg vrequest_mt[] = {
	{ "__index", lua_vrequest_index },
	{ "__newindex", lua_vrequest_newindex },

	{ "print", lua_vrequest_error },
	{ "warning", lua_vrequest_warning },
	{ "info", lua_vrequest_info },
	{ "debug", lua_vrequest_debug },

	{ "stat", lua_vrequest_stat },

	{ "handle_direct", lua_vrequest_handle_direct },

	{ "enter_action", lua_vrequest_enter_action },

	{ "add_filter_in", lua_vrequest_add_filter_in },
	{ "add_filter_out", lua_vrequest_add_filter_out },

	{ "subrequest", li_lua_vrequest_subrequest },

	{ NULL, NULL }
};

static HEDLEY_NEVER_INLINE void init_vrequest_mt(lua_State *L) {
	li_lua_setfuncs(L, vrequest_mt);
}

static void lua_push_vrequest_metatable(lua_State *L) {
	if (li_lua_new_protected_metatable(L, LUA_VREQUEST)) {
		init_vrequest_mt(L);
	}
}

liVRequest* li_lua_get_vrequest(lua_State *L, int ndx) {
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

int li_lua_push_vrequest(lua_State *L, liVRequest *vr) {
	liVRequest **pvr;

	if (NULL == vr) {
		lua_pushnil(L);
		return 1;
	}

	pvr = (liVRequest**) lua_newuserdata(L, sizeof(liVRequest*));
	*pvr = vr;

	lua_push_vrequest_metatable(L);
	lua_setmetatable(L, -2);
	return 1;
}


#define LUA_CONINFO "liConInfo*"

typedef int (*lua_ConInfo_Attrib)(liConInfo *coninfo, lua_State *L);

static int lua_coninfo_attr_read_local(liConInfo *coninfo, lua_State *L) {
	lua_pushlstring(L, GSTR_LEN(coninfo->local_addr_str));
	return 1;
}

static int lua_coninfo_attr_read_remote(liConInfo *coninfo, lua_State *L) {
	lua_pushlstring(L, GSTR_LEN(coninfo->remote_addr_str));
	return 1;
}

#define AR(m) { #m, lua_coninfo_attr_read_##m, NULL }
#define AW(m) { #m, NULL, lua_coninfo_attr_write_##m }
#define ARW(m) { #m, lua_coninfo_attr_read_##m, lua_coninfo_attr_write_##m }

static const struct {
	const char* key;
	lua_ConInfo_Attrib read_attr, write_attr;
} coninfo_attribs[] = {
	AR(local),
	AR(remote),

	{ NULL, NULL, NULL }
};

#undef AR
#undef AW
#undef ARW

static int lua_coninfo_index(lua_State *L) {
	liConInfo *coninfo;
	const char *key;
	int i;

	if (lua_gettop(L) != 2) {
		lua_pushstring(L, "incorrect number of arguments");
		lua_error(L);
	}

	if (li_lua_metatable_index(L)) return 1;

	coninfo = li_lua_get_coninfo(L, 1);
	if (!coninfo) return 0;

	if (lua_isnumber(L, 2)) return 0;
	if (!lua_isstring(L, 2)) return 0;

	key = lua_tostring(L, 2);
	for (i = 0; coninfo_attribs[i].key ; i++) {
		if (0 == strcmp(key, coninfo_attribs[i].key)) {
			if (coninfo_attribs[i].read_attr)
				return coninfo_attribs[i].read_attr(coninfo, L);
			break;
		}
	}

	lua_pushstring(L, "cannot read attribute ");
	lua_pushstring(L, key);
	lua_pushstring(L, " in coninfo");
	lua_concat(L, 3);
	lua_error(L);

	return 0;
}

static int lua_coninfo_newindex(lua_State *L) {
	liConInfo *coninfo;
	const char *key;
	int i;

	if (lua_gettop(L) != 3) {
		lua_pushstring(L, "incorrect number of arguments");
		lua_error(L);
	}

	coninfo = li_lua_get_coninfo(L, 1);
	if (!coninfo) return 0;

	if (lua_isnumber(L, 2)) return 0;
	if (!lua_isstring(L, 2)) return 0;

	key = lua_tostring(L, 2);
	for (i = 0; coninfo_attribs[i].key ; i++) {
		if (0 == strcmp(key, coninfo_attribs[i].key)) {
			if (coninfo_attribs[i].write_attr)
				return coninfo_attribs[i].write_attr(coninfo, L);
			break;
		}
	}

	lua_pushstring(L, "cannot write attribute ");
	lua_pushstring(L, key);
	lua_pushstring(L, "in coninfo");
	lua_concat(L, 3);
	lua_error(L);

	return 0;
}

static const luaL_Reg coninfo_mt[] = {
	{ "__index", lua_coninfo_index },
	{ "__newindex", lua_coninfo_newindex },

	{ NULL, NULL }
};

static HEDLEY_NEVER_INLINE void init_coninfo_mt(lua_State *L) {
	li_lua_setfuncs(L, coninfo_mt);
}

static void lua_push_coninfo_metatable(lua_State *L) {
	if (li_lua_new_protected_metatable(L, LUA_CONINFO)) {
		init_coninfo_mt(L);
	}
}

void li_lua_init_virtualrequest_mt(lua_State *L) {
	lua_push_vrequest_metatable(L);
	lua_pop(L, 1);

	lua_push_coninfo_metatable(L);
	lua_pop(L, 1);
}

liConInfo* li_lua_get_coninfo(lua_State *L, int ndx) {
	if (!lua_isuserdata(L, ndx)) return NULL;
	if (!lua_getmetatable(L, ndx)) return NULL;
	luaL_getmetatable(L, LUA_CONINFO);
	if (lua_isnil(L, -1) || lua_isnil(L, -2) || !lua_equal(L, -1, -2)) {
		lua_pop(L, 2);
		return NULL;
	}
	lua_pop(L, 2);
	return *(liConInfo**) lua_touserdata(L, ndx);
}

int li_lua_push_coninfo(lua_State *L, liConInfo *coninfo) {
	liConInfo **pconinfo;

	if (NULL == coninfo) {
		lua_pushnil(L);
		return 1;
	}

	pconinfo = (liConInfo**) lua_newuserdata(L, sizeof(liConInfo*));
	*pconinfo = coninfo;

	lua_push_coninfo_metatable(L);
	lua_setmetatable(L, -2);
	return 1;
}

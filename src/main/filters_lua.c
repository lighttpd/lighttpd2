
#include <lighttpd/base.h>
#include <lighttpd/core_lua.h>
#include <lighttpd/actions_lua.h>

#include <lualib.h>
#include <lauxlib.h>

#define LUA_FILTER "liFilter*"

typedef int (*lua_Filter_Attrib)(liFilter *f, lua_State *L);

static int lua_filter_attr_read_in(liFilter *f, lua_State *L) {
	li_lua_push_chunkqueue(L, f->in);
	return 1;
}

static int lua_filter_attr_read_out(liFilter *f, lua_State *L) {
	li_lua_push_chunkqueue(L, f->out);
	return 1;
}

#define AR(m) { #m, lua_filter_attr_read_##m, NULL }
#define AW(m) { #m, NULL, lua_filter_attr_write_##m }
#define ARW(m) { #m, lua_filter_attr_read_##m, lua_filter_attr_write_##m }

static const struct {
	const char* key;
	lua_Filter_Attrib read_attr, write_attr;
} filter_attribs[] = {
	AR(in),
	AR(out),

	{ NULL, NULL, NULL }
};

static int lua_filter_index(lua_State *L) {
	liFilter *f;
	const char *key;
	int i;

	if (lua_gettop(L) != 2) {
		lua_pushstring(L, "incorrect number of arguments");
		lua_error(L);
	}

	if (li_lua_metatable_index(L)) return 1;

	f = li_lua_get_filter(L, 1);
	if (!f) return 0;

	if (lua_isnumber(L, 2)) return 0;
	if (!lua_isstring(L, 2)) return 0;

	key = lua_tostring(L, 2);
	for (i = 0; filter_attribs[i].key ; i++) {
		if (0 == strcmp(key, filter_attribs[i].key)) {
			if (filter_attribs[i].read_attr)
				return filter_attribs[i].read_attr(f, L);
			break;
		}
	}

	lua_pushstring(L, "cannot read attribute ");
	lua_pushstring(L, key);
	lua_pushstring(L, " in filter");
	lua_concat(L, 3);
	lua_error(L);

	return 0;
}

static int lua_filter_newindex(lua_State *L) {
	liFilter *f;
	const char *key;
	int i;

	if (lua_gettop(L) != 3) {
		lua_pushstring(L, "incorrect number of arguments");
		lua_error(L);
	}

	f = li_lua_get_filter(L, 1);
	if (!f) return 0;

	if (lua_isnumber(L, 2)) return 0;
	if (!lua_isstring(L, 2)) return 0;

	key = lua_tostring(L, 2);
	for (i = 0; filter_attribs[i].key ; i++) {
		if (0 == strcmp(key, filter_attribs[i].key)) {
			if (filter_attribs[i].write_attr)
				return filter_attribs[i].write_attr(f, L);
			break;
		}
	}

	lua_pushstring(L, "cannot write attribute ");
	lua_pushstring(L, key);
	lua_pushstring(L, "in filter");
	lua_concat(L, 3);
	lua_error(L);

	return 0;
}

static const luaL_Reg filter_mt[] = {
	{ "__index", lua_filter_index },
	{ "__newindex", lua_filter_newindex },

	{ NULL, NULL }
};

static void init_filter_mt(lua_State *L) {
	luaL_register(L, NULL, filter_mt);
}

void li_lua_init_filter_mt(lua_State *L) {
	if (luaL_newmetatable(L, LUA_FILTER)) {
		init_filter_mt(L);
	}
	lua_pop(L, 1);
}

liFilter* li_lua_get_filter(lua_State *L, int ndx) {
	if (!lua_isuserdata(L, ndx)) return NULL;
	if (!lua_getmetatable(L, ndx)) return NULL;
	luaL_getmetatable(L, LUA_FILTER);
	if (lua_isnil(L, -1) || lua_isnil(L, -2) || !lua_equal(L, -1, -2)) {
		lua_pop(L, 2);
		return NULL;
	}
	lua_pop(L, 2);
	return *(liFilter**) lua_touserdata(L, ndx);
}

int li_lua_push_filter(lua_State *L, liFilter *f) {
	liFilter **pf;

	pf = (liFilter**) lua_newuserdata(L, sizeof(liFilter*));
	*pf = f;

	if (luaL_newmetatable(L, LUA_FILTER)) {
		init_filter_mt(L);
	}

	lua_setmetatable(L, -2);
	return 1;
}



typedef struct filter_lua_config filter_lua_config;
struct filter_lua_config {
	liLuaState *LL;
	int class_ref;
};

typedef struct filter_lua_state filter_lua_state;
struct filter_lua_state {
	liLuaState *LL;
	int object_ref;
};

static filter_lua_state* filter_lua_state_new(liVRequest *vr, filter_lua_config *config) {
	int object_ref = LUA_NOREF;
	liServer *srv = vr->wrk->srv;
	lua_State *L = config->LL->L;

	li_lua_lock(config->LL);

	lua_rawgeti(L, LUA_REGISTRYINDEX, config->class_ref); /* +1 */
	li_lua_push_vrequest(L, vr); /* +1 */

	if (li_lua_call_object(srv, vr, L, "new", 2, 1, FALSE)) { /* -2, +1 on success */
		if (!lua_isnil(L, -1)) {
			object_ref = luaL_ref(L, LUA_REGISTRYINDEX); /* -1 */
		} else { /* no error; nil is interpreted as "don't need this filter for this request" */
			lua_pop(L, 1); /* -1 */
		}
	} else {
		VR_ERROR(vr, "%s", "li_lua_call_object failed");
		li_vrequest_error(vr);
	}

	li_lua_unlock(config->LL);

	if (LUA_NOREF != object_ref) {
		filter_lua_state *state = g_slice_new0(filter_lua_state);
		state->LL = config->LL;
		state->object_ref = object_ref;

		return state;
	} else {
		return NULL;
	}
}

static void filter_lua_state_free(liVRequest *vr, filter_lua_state *state) {
	liServer *srv = vr->wrk->srv;
	lua_State *L = state->LL->L;

	li_lua_lock(state->LL);

	lua_rawgeti(L, LUA_REGISTRYINDEX, state->object_ref); /* +1 */
	li_lua_push_vrequest(L, vr); /* +1 */
	li_lua_call_object(srv, vr, L, "finished", 2, 0, TRUE); /* -2 */

	luaL_unref(L, LUA_REGISTRYINDEX, state->object_ref);

	li_lua_unlock(state->LL);

	g_slice_free(filter_lua_state, state);
}

static void filter_lua_free(liVRequest *vr, liFilter *f) {
	filter_lua_state *state = (filter_lua_state*) f->param;

	filter_lua_state_free(vr, state);
}

static liHandlerResult filter_lua_handle(liVRequest *vr, liFilter *f) {
	filter_lua_state *state = (filter_lua_state*) f->param;
	lua_State *L = state->LL->L;
	liHandlerResult res;

	li_lua_lock(state->LL);

	lua_rawgeti(L, LUA_REGISTRYINDEX, state->object_ref); /* +1 */
	li_lua_push_vrequest(L, vr); /* +1 */
	li_lua_push_chunkqueue(L, f->out); /* +1 */
	li_lua_push_chunkqueue(L, f->in); /* +1 */
	if (li_lua_call_object(NULL, vr, L, "handle", 4, 1, FALSE)) { /* -4, +1 on success */
		res = LI_HANDLER_GO_ON;
		if (!lua_isnil(L, -1)) {
			int rc = lua_tointeger(L, -1);
			switch (rc) {
			case LI_HANDLER_GO_ON:
			case LI_HANDLER_COMEBACK:
			case LI_HANDLER_WAIT_FOR_EVENT:
			case LI_HANDLER_ERROR:
				res = rc;
				break;
			default:
				VR_ERROR(vr, "lua filter returned an error or an unknown value (%i)", rc);
				res = LI_HANDLER_ERROR;
			}
		}
		lua_pop(L, 1);
	} else {
		res = LI_HANDLER_ERROR;
	}

	li_lua_unlock(state->LL);

	return res;
}

static liHandlerResult filter_lua_in(liVRequest *vr, gpointer param, gpointer *context) {
	filter_lua_config *config = param;
	filter_lua_state *state = filter_lua_state_new(vr, config);
	UNUSED(context);

	if (state) {
		li_vrequest_add_filter_in(vr, filter_lua_handle, filter_lua_free, NULL, state);
	}

	return LI_HANDLER_GO_ON;
}

static liHandlerResult filter_lua_out(liVRequest *vr, gpointer param, gpointer *context) {
	filter_lua_config *config = param;
	filter_lua_state *state = filter_lua_state_new(vr, config);
	UNUSED(context);

	if (state) {
		li_vrequest_add_filter_out(vr, filter_lua_handle, filter_lua_free, NULL, state);
	}

	return LI_HANDLER_GO_ON;
}

static void filter_lua_action_free(liServer *srv, gpointer param) {
	filter_lua_config *config = param;
	lua_State *L = config->LL->L;
	UNUSED(srv);

	li_lua_lock(config->LL);
	luaL_unref(L, LUA_REGISTRYINDEX, config->class_ref);
	li_lua_unlock(config->LL);

	g_slice_free(filter_lua_config, config);
}

static int filter_lua_action_create(lua_State *L, liActionFuncCB act_cb) {
	liLuaState *LL = li_lua_state_get(L);
	liServer *srv = lua_touserdata(L, lua_upvalueindex(1));
	liAction *act;
	filter_lua_config *config;

	if (lua_gettop(L) != 1 || lua_isnil(L, 1)) {
		int n = lua_gettop(L);
		lua_pushstring(L, "expected exactly one parameter for lighty.filter_[in/out], got ");
		lua_pushinteger(L, n);
		lua_concat(L, 2);
		return lua_error(L);
	}

	config = g_slice_new0(filter_lua_config);
	config->LL = LL;
	config->class_ref = luaL_ref(L, LUA_REGISTRYINDEX);

	act = li_action_new_function(act_cb, NULL, filter_lua_action_free, config);
	return li_lua_push_action(srv, L, act);
}

static int filter_lua_in_create(lua_State *L) {
	return filter_lua_action_create(L, filter_lua_in);
}

static int filter_lua_out_create(lua_State *L) {
	return filter_lua_action_create(L, filter_lua_out);
}

void li_lua_init_filters(lua_State *L, liServer* srv) {
	lua_pushlightuserdata(L, srv);
	lua_pushcclosure(L, filter_lua_in_create, 1);
	lua_setfield(L, -2, "filter_in");

	lua_pushlightuserdata(L, srv);
	lua_pushcclosure(L, filter_lua_out_create, 1);
	lua_setfield(L, -2, "filter_out");
}

liFilter* li_lua_vrequest_add_filter_in(lua_State *L, liVRequest *vr, int state_ndx) {
	filter_lua_state *state;
	liLuaState *LL = li_lua_state_get(L);

	lua_pushvalue(L, state_ndx);

	state = g_slice_new0(filter_lua_state);
	state->LL = LL;
	state->object_ref = luaL_ref(L, LUA_REGISTRYINDEX);

	return li_vrequest_add_filter_in(vr, filter_lua_handle, filter_lua_free, NULL, state);
}

liFilter* li_lua_vrequest_add_filter_out(lua_State *L, liVRequest *vr, int state_ndx) {
	filter_lua_state *state;
	liLuaState *LL = li_lua_state_get(L);

	lua_pushvalue(L, state_ndx);

	state = g_slice_new0(filter_lua_state);
	state->LL = LL;
	state->object_ref = luaL_ref(L, LUA_REGISTRYINDEX);

	return li_vrequest_add_filter_out(vr, filter_lua_handle, filter_lua_free, NULL, state);
}

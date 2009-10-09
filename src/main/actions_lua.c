
#include <lighttpd/actions_lua.h>
#include <lighttpd/core_lua.h>

#include <lualib.h>
#include <lauxlib.h>

#define LUA_ACTION "liAction*"

liAction* li_lua_get_action(lua_State *L, int ndx) {
	if (!lua_isuserdata(L, ndx)) return NULL;
	if (!lua_getmetatable(L, ndx)) return NULL;
	luaL_getmetatable(L, LUA_ACTION);
	if (lua_isnil(L, -1) || lua_isnil(L, -2) || !lua_equal(L, -1, -2)) {
		lua_pop(L, 2);
		return NULL;
	}
	lua_pop(L, 2);
	return *(liAction**) lua_touserdata(L, ndx);
}

static int lua_action_gc(lua_State *L) {
	liServer *srv;
	liAction **a = (liAction**) luaL_checkudata(L, 1, LUA_ACTION);
	if (!a || !*a) return 0;

	srv = (liServer*) lua_touserdata(L, lua_upvalueindex(1));
	li_action_release(srv, *a);
	return 0;
}

int li_lua_push_action(liServer *srv, lua_State *L, liAction *a) {
	liAction **pa;

	pa = (liAction**) lua_newuserdata(L, sizeof(liAction*));
	*pa = a;

	if (luaL_newmetatable(L, LUA_ACTION)) {
		lua_pushlightuserdata(L, srv);
		lua_pushcclosure(L, lua_action_gc, 1);
		lua_setfield(L, -2, "__gc");
	}

	lua_setmetatable(L, -2);
	return 1;
}

typedef struct lua_action_param lua_action_param;
struct lua_action_param {
	int func_ref;
	lua_State *L;
};

typedef struct lua_action_ctx lua_action_ctx;
struct lua_action_ctx {
	int g_ref;
};

static liHandlerResult lua_action_func(liVRequest *vr, gpointer param, gpointer *context) {
	lua_action_param *par = param;
	lua_action_ctx *ctx = *context;
	liServer *srv = vr->wrk->srv;
	lua_State *L = par->L;
	gboolean dolock = (L == srv->L);
	liHandlerResult res = LI_HANDLER_GO_ON;
	int errfunc;

	if (dolock) li_lua_lock(srv);

	lua_rawgeti(L, LUA_REGISTRYINDEX, par->func_ref);
	lua_pushvalue(L, -1);

	lua_getfenv(L, -1);
	if (!ctx) {
		*context = ctx = g_slice_new0(lua_action_ctx);
		lua_newtable(L);
		lua_pushvalue(L, -1);
		ctx->g_ref = luaL_ref(L, LUA_REGISTRYINDEX);
	} else {
		lua_rawgeti(L, LUA_REGISTRYINDEX, ctx->g_ref);
	}
	lua_setfield(L, -2, "_G");
	lua_pop(L, 1);

	li_lua_push_vrequest(L, vr);

	errfunc = li_lua_push_traceback(L, 1);
	if (lua_pcall(L, 1, 1, errfunc)) {
		ERROR(srv, "lua_pcall(): %s", lua_tostring(L, -1));
		lua_pop(L, 1);
		res = LI_HANDLER_ERROR;
	} else {
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
				res = LI_HANDLER_ERROR;
			}
		}
		lua_pop(L, 1);
	}
	lua_remove(L, errfunc);

	lua_getfenv(L, -1);
	lua_pushnil(L);
	lua_setfield(L, -2, "_G");
	lua_pop(L, 2);

	if (dolock) li_lua_unlock(srv);

	return res;
}

static liHandlerResult lua_action_cleanup(liVRequest *vr, gpointer param, gpointer context) {
	lua_action_param *par = param;
	lua_action_ctx *ctx = context;
	liServer *srv = vr->wrk->srv;
	lua_State *L = par->L;
	gboolean dolock = (L == srv->L);

	if (dolock) li_lua_lock(srv);
	luaL_unref(L, LUA_REGISTRYINDEX, ctx->g_ref);
	if (dolock) li_lua_unlock(srv);

	g_slice_free(lua_action_ctx, ctx);

	return LI_HANDLER_GO_ON;
}

static void lua_action_free(liServer *srv, gpointer param) {
	lua_action_param *par = param;
	lua_State *L = par->L;
	gboolean dolock = (L == srv->L);

	if (dolock) li_lua_lock(srv);
	luaL_unref(L, LUA_REGISTRYINDEX, par->func_ref);
	if (dolock) li_lua_unlock(srv);

	g_slice_free(lua_action_param, par);
}

liAction* li_lua_make_action(lua_State *L, int ndx) {
	lua_action_param *par = g_slice_new0(lua_action_param);

	lua_pushvalue(L, ndx); /* +1 */
	par->func_ref = luaL_ref(L, LUA_REGISTRYINDEX); /* -1 */
	par->L = L;

	/* new environment for function */
	lua_pushvalue(L, ndx); /* +1 */
	lua_newtable(L); /* +1 */
		/* new mt */
		lua_newtable(L); /* +1 */
		lua_getfield(L, LUA_REGISTRYINDEX, "li_globals"); /* +1 */
		lua_setfield(L, -2, "__index"); /* -1 */
		lua_setmetatable(L, -2); /* -1 */
	lua_setfenv(L, -2); /* -1 */
	lua_pop(L, 1); /* -1 */

	return li_action_new_function(lua_action_func, lua_action_cleanup, lua_action_free, par);
}


#include <lighttpd/actions_lua.h>
#include <lighttpd/config_lua.h>
#include <lighttpd/core_lua.h>

#define LUA_ACTION "liAction*"

static int lua_action_gc(lua_State *L) {
	liServer *srv;
	liLuaState *LL = li_lua_state_get(L);
	liAction **a = (liAction**) luaL_checkudata(L, 1, LUA_ACTION);
	if (!a || !*a) return 0;

	srv = (liServer*) lua_touserdata(L, lua_upvalueindex(1));

	li_lua_unlock(LL);
	li_action_release(srv, *a);
	li_lua_lock(LL);

	return 0;
}

static HEDLEY_NEVER_INLINE void init_action_mt(liServer *srv, lua_State *L) {
	lua_pushlightuserdata(L, srv);
	lua_pushcclosure(L, lua_action_gc, 1);
	lua_setfield(L, -2, "__gc");
}

static void lua_push_action_metatable(liServer *srv, lua_State *L) {
	if (li_lua_new_protected_metatable(L, LUA_ACTION)) {
		init_action_mt(srv, L);
	}
}

void li_lua_init_action_mt(liServer *srv, lua_State *L) {
	lua_push_action_metatable(srv, L);
	lua_pop(L, 1);
}

liAction* li_lua_get_action(lua_State *L, int ndx) {
	if (!lua_isuserdata(L, ndx)) return NULL;
	if (!lua_getmetatable(L, ndx)) return NULL;
	luaL_getmetatable(L, LUA_ACTION);
	if (lua_isnil(L, -1) || lua_isnil(L, -2) || !li_lua_equal(L, -1, -2)) {
		lua_pop(L, 2);
		return NULL;
	}
	lua_pop(L, 2);
	return *(liAction**) lua_touserdata(L, ndx);
}

int li_lua_push_action(liServer *srv, lua_State *L, liAction *a) {
	liAction **pa;

	if (NULL == a) {
		lua_pushnil(L);
		return 1;
	}

	pa = (liAction**) lua_newuserdata(L, sizeof(liAction*));
	*pa = a;

	lua_push_action_metatable(srv, L);
	lua_setmetatable(L, -2);
	return 1;
}

typedef struct lua_action_param lua_action_param;
struct lua_action_param {
	int func_ref;
	liLuaState *LL;
};

typedef struct lua_action_ctx lua_action_ctx;
struct lua_action_ctx {
	int g_ref;
};

static liHandlerResult lua_action_func(liVRequest *vr, gpointer param, gpointer *context) {
	lua_action_param *par = param;
	lua_action_ctx *ctx = *context;
	liServer *srv = vr->wrk->srv;
	lua_State *L = par->LL->L;
	liHandlerResult res = LI_HANDLER_GO_ON;
	int errfunc;

	li_lua_lock(par->LL);

	if (!ctx) {
		*context = ctx = g_slice_new0(lua_action_ctx);
		ctx->g_ref = li_lua_environment_create(par->LL, vr);
	}
	li_lua_environment_activate(par->LL, ctx->g_ref); /* +1 */

	lua_rawgeti(L, LUA_REGISTRYINDEX, par->func_ref); /* +1 */
	li_lua_push_vrequest(L, vr); /* +1 */

	errfunc = li_lua_push_traceback(L, 1); /* +1, but inserted before func and 1 arg */
	if (lua_pcall(L, 1, 1, errfunc)) { /* -2 (func + arg), +1 (result / error) */
		ERROR(srv, "lua_pcall(): %s", lua_tostring(L, -1));
		lua_pop(L, 1); /* -1 (pop error) */
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
		lua_pop(L, 1); /* -1 (pop result) */
	}
	lua_remove(L, errfunc); /* -1 (should be at the top of the stack) */

	li_lua_environment_restore(par->LL); /* -1 */

	li_lua_unlock(par->LL);

	return res;
}

static liHandlerResult lua_action_cleanup(liVRequest *vr, gpointer param, gpointer context) {
	lua_action_param *par = param;
	lua_action_ctx *ctx = context;
	lua_State *L = par->LL->L;
	UNUSED(vr);

	li_lua_lock(par->LL);
	luaL_unref(L, LUA_REGISTRYINDEX, ctx->g_ref);
	li_lua_unlock(par->LL);

	g_slice_free(lua_action_ctx, ctx);

	return LI_HANDLER_GO_ON;
}

static void lua_action_free(liServer *srv, gpointer param) {
	lua_action_param *par = param;
	lua_State *L;
	UNUSED(srv);

	if (!param) return;

	L = par->LL->L;

	li_lua_lock(par->LL);
	luaL_unref(L, LUA_REGISTRYINDEX, par->func_ref);
	lua_gc(L, LUA_GCCOLLECT, 0);
	li_lua_unlock(par->LL);

	g_slice_free(lua_action_param, par);
}

liAction* li_lua_make_action(lua_State *L, int ndx) {
	lua_action_param *par = g_slice_new0(lua_action_param);

	lua_pushvalue(L, ndx); /* +1 */
	par->func_ref = luaL_ref(L, LUA_REGISTRYINDEX); /* -1 */
	par->LL = li_lua_state_get(L);

	return li_action_new_function(lua_action_func, lua_action_cleanup, lua_action_free, par);
}

liAction* li_lua_get_action_ref(lua_State *L, int ndx) {
	liAction *act;

	act = li_lua_get_action(L, ndx);
	if (NULL == act) {
		if (lua_isfunction(L, ndx)) {
			act = li_lua_make_action(L, ndx);
		}
	} else {
		li_action_acquire(act);
	}

	return act;
}

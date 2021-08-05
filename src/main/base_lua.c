
#include <lighttpd/base.h>

#ifdef HAVE_LUA_H
# include <lighttpd/core_lua.h>
# include <lualib.h>
#endif

#ifdef HAVE_LUA_H

void li_lua_init(liLuaState* LL, liServer* srv, liWorker* wrk) {
	lua_State *L = LL->L = luaL_newstate();

	lua_pushlightuserdata(L, LL);
	lua_setfield(L, LUA_REGISTRYINDEX, LI_LUA_REGISTRY_STATE);

	luaL_openlibs(LL->L);
	li_lua_init2(LL, srv, wrk);

	g_static_rec_mutex_init(&LL->lualock);
}

void li_lua_clear(liLuaState* LL) {
	lua_close(LL->L);
	LL->L = NULL;

	g_static_rec_mutex_free(&LL->lualock);
}

#else

void li_lua_init(liLuaState* LL, liServer* srv, liWorker* wrk) {
	UNUSED(srv);
	UNUSED(wrk);

	LL->L = NULL;
	g_static_rec_mutex_init(&LL->lualock);
}

void li_lua_clear(liLuaState* LL) {
	g_static_rec_mutex_free(&LL->lualock);
}

#endif

#ifndef _LIGHTTPD_BASE_LUA_H_
#define _LIGHTTPD_BASE_LUA_H_

#ifndef _LIGHTTPD_BASE_H_
#error Please include <lighttpd/base.h> instead of this file
#endif

/* this file defines lighttpd <-> lua glue which is always active, even if compiled without lua */

struct lua_State;

struct liLuaState {
	struct lua_State* L; /** NULL if compiled without Lua */
	GStaticRecMutex lualock;
};

LI_API void li_lua_init(liLuaState* LL, liServer* srv, liWorker* wrk);
LI_API void li_lua_clear(liLuaState* LL);

#endif

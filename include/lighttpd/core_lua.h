#ifndef _LIGHTTPD_CORE_LUA_H_
#define _LIGHTTPD_CORE_LUA_H_

#include <lighttpd/base.h>
#include <lua.h>
#include <lauxlib.h>

#define LI_LUA_REGISTRY_STATE   "lighttpd.state"
#define LI_LUA_REGISTRY_SERVER  "lighttpd.server"
#define LI_LUA_REGISTRY_WORKER  "lighttpd.worker"
#define LI_LUA_REGISTRY_GLOBALS "lighttpd.globals"

LI_API liLuaState *li_lua_state_get(lua_State *L);

INLINE void li_lua_lock(liLuaState *LL);
INLINE void li_lua_unlock(liLuaState *LL);

/* expect (meta)table at top of the stack */
INLINE void li_lua_protect_metatable(lua_State *L);
INLINE int li_lua_new_protected_metatable(lua_State *L, const char *tname);

/* chunk_lua.c */
LI_API void li_lua_init_chunk_mt(lua_State *L);

LI_API liChunk* li_lua_get_chunk(lua_State *L, int ndx);
LI_API int li_lua_push_chunk(lua_State *L, liChunk *c);
LI_API liChunkQueue* li_lua_get_chunkqueue(lua_State *L, int ndx);
LI_API int li_lua_push_chunkqueue(lua_State *L, liChunkQueue *cq);

/* environment_lua.c */
LI_API void li_lua_init_environment_mt(lua_State *L);

LI_API liEnvironment* li_lua_get_environment(lua_State *L, int ndx);
LI_API int li_lua_push_environment(lua_State *L, liEnvironment *env);

/* filters_lua.c */
LI_API void li_lua_init_filter_mt(lua_State *L);
/* create entries in `lighty.` table, which must be on top of the stack */
LI_API void li_lua_init_filters(lua_State *L, liServer* srv);

LI_API liFilter* li_lua_get_filter(lua_State *L, int ndx);
LI_API int li_lua_push_filter(lua_State *L, liFilter *f);
LI_API liFilter* li_lua_vrequest_add_filter_in(lua_State *L, liVRequest *vr, int state_ndx);
LI_API liFilter* li_lua_vrequest_add_filter_out(lua_State *L, liVRequest *vr, int state_ndx);

/* http_headers_lua.c */
LI_API void li_lua_init_http_headers_mt(lua_State *L);

LI_API liHttpHeaders* li_lua_get_http_headers(lua_State *L, int ndx);
LI_API int li_lua_push_http_headers(lua_State *L, liHttpHeaders *headers);

/* physical_lua.c */
LI_API void li_lua_init_physical_mt(lua_State *L);

LI_API liPhysical* li_lua_get_physical(lua_State *L, int ndx);
LI_API int li_lua_push_physical(lua_State *L, liPhysical *phys);

/* request_lua.c */
LI_API void li_lua_init_request_mt(lua_State *L);

LI_API liRequest* li_lua_get_request(lua_State *L, int ndx);
LI_API int li_lua_push_request(lua_State *L, liRequest *req);

LI_API liRequestUri* li_lua_get_requesturi(lua_State *L, int ndx);
LI_API int li_lua_push_requesturi(lua_State *L, liRequestUri *uri);

/* response_lua.c */
LI_API void li_lua_init_response_mt(lua_State *L);

LI_API liResponse* li_lua_get_response(lua_State *L, int ndx);
LI_API int li_lua_push_response(lua_State *L, liResponse *resp);

/* stat_lua.c */
LI_API void li_lua_init_stat_mt(lua_State *L);

LI_API struct stat* li_lua_get_stat(lua_State *L, int ndx);
LI_API int li_lua_push_stat(lua_State *L, struct stat *st);

/* subrequest_lua.c */
LI_API void li_lua_init_subrequest_mt(lua_State *L);

/* virtualrequest_lua.c */
LI_API void li_lua_init_virtualrequest_mt(lua_State *L);

LI_API liVRequest* li_lua_get_vrequest(lua_State *L, int ndx);
LI_API int li_lua_push_vrequest(lua_State *L, liVRequest *vr);

LI_API liConInfo* li_lua_get_coninfo(lua_State *L, int ndx);
LI_API int li_lua_push_coninfo(lua_State *L, liConInfo *vr);

/* everything else: core_lua.c */

LI_API int li_lua_fixindex(lua_State *L, int ndx);

/* return 1 if value is found in mt (on top of the stack), 0 if it is not found (stack balance = 0)
 * table, key on stack at pos 0 and 1 (i.e. __index method)
 */
LI_API int li_lua_metatable_index(lua_State *L);

LI_API void li_lua_init2(liLuaState* LL, liServer* srv, liWorker* wrk);

LI_API int li_lua_push_traceback(lua_State *L, int nargs);

/* nargs: number of arguments *with* object; object must be the first of the arguments
 * returns: FALSE: an error occurred. stack balance -nargs (i.e. popped args)
 *          TRUE: stack balance nresp-narg; popped args, pushed results
 * srv/vr are only needed for error logging
 * if optional is TRUE don't log an error if the method doesn't exist
 */
LI_API gboolean li_lua_call_object(liServer *srv, liVRequest *vr, lua_State *L, const char* method, int nargs, int nresults, gboolean optional);

/* Manage environment ("globals", `_ENV`, ...)
 * - by default globals shouldn't survive across requests (and not from initial execution either)
 * - globals set during a request should be "local" to request and context (per handler, filter, ...)
 * - only overwrite environment "locally" for our code
 * - inherit normal environment through metatables (just setting globals is "local")
 * - `_G` is inherited and can be used for persistent state.
 * - `REQ` in request context points to a "request-global" table (per lua state of course)
 * - lua5.2+: can use _ENV.x to make access to global "x" explicit
 *
 * modifying existing objects in environment (inherited from "main" global environment)
 * are persisted though (per lua state).
 *
 * Implementation:
 * - remember initial GLOBALS in liLuaState
 * - single instance of "our environment" table LI_ENV; "empty" userdata with (initial) metatable LI_ENV_MT_DEFAULT={__index=GLOBALS}
 *   * creating new globals would fail - not designed for actual use
 * - "ephemeral" environment for code running outside of requests:
     * change LI_ENV metatable to {__index=__newindex={} with metatable LI_ENV_MT_DEFAULT }
     * globals lost once environment is "restored"
 * - "request" environment for code running during a requests:
     * create REQ_CTX table with metatable LI_ENV_MT_DEFAULT for lifetime of request
     * change LI_ENV metatable to {__index=__newindex=REQ_CTX }
     * globals lost once request is cleaned up
 * - restoring environment:
     restore LI_ENV metatable to previous meta table (index returned by active methods)
 */
LI_API void li_lua_environment_activate_ephemeral(liLuaState *LL); /* +1 */
LI_API int li_lua_environment_create(liLuaState *LL, liVRequest *vr); /* returns env (metatable) ref */
INLINE void li_lua_environment_free(lua_State *L, int env_mt_ref);
LI_API void li_lua_environment_activate(liLuaState *LL, int env_mt_ref); /* +1 */
LI_API void li_lua_environment_restore(liLuaState *LL); /* -1 */

/* make LI_ENV the current global environment (backup old environment on stack) */
LI_API void li_lua_environment_use_globals(liLuaState *LL); /* +1 */
/* restore previous global environment from stack */
LI_API void li_lua_environment_restore_globals(lua_State *L); /* -1 */

/* joinWith " " (map tostring parameter[from..to]) */
LI_API GString* li_lua_print_get_string(lua_State *L, int from, int to);

/* pairs() for a GHashTable GString -> GString:
 *   Don't modify the hashtable while iterating:
 *   - no new keys
 *   - no delete
 *  Modifying values is fine; g_hash_table_insert() as long as the key already exists too.
 * The returned "next" function has an internal state, it ignores the table/state and previous key parameter.
 * returns: <next>, nil, nil on the stack (and 3 as c function)
 */
LI_API int li_lua_ghashtable_gstring_pairs(lua_State *L, GHashTable *ht);

/* internal: subrequests (vrequest metamethod) */
LI_API int li_lua_vrequest_subrequest(lua_State *L);


/* inline implementations */

INLINE void li_lua_lock(liLuaState *LL) {
	gboolean b;
	g_static_rec_mutex_lock(&LL->lualock);
	b = lua_checkstack(LL->L, LUA_MINSTACK);
	LI_FORCE_ASSERT(b);
}

INLINE void li_lua_unlock(liLuaState *LL) {
	g_static_rec_mutex_unlock(&LL->lualock);
}

INLINE void li_lua_protect_metatable(lua_State *L) {
	/* __metatable key prevents accessing metatable of objects/tables in normal lua code */
	lua_pushboolean(L, FALSE);
	lua_setfield(L, -2, "__metatable");
}

INLINE int li_lua_new_protected_metatable(lua_State *L, const char *tname) {
	int r = luaL_newmetatable(L, tname);
	if (r) {
		li_lua_protect_metatable(L);
	}
	return r;
}

/* create "stable" stack index from negative offsets like -1 and -2 (but not pseudo-indices) */
INLINE int li_lua_stable_index(lua_State *L, int index) {
	if (index < 0) {
		int top = lua_gettop(L);
		/* valid iff: 1 <= abs(index) = -index <= top; "-index" might overflow, so check `index >= -top` instead */
		if (index >= -top) return (top + index) + 1;
	}
	return index;
}

INLINE void li_lua_environment_free(lua_State *L, int env_mt_ref) {
	luaL_unref(L, LUA_REGISTRYINDEX, env_mt_ref);
}

INLINE void li_lua_setfuncs(lua_State *L, const luaL_Reg *l) {
#if LUA_VERSION_NUM == 501
	luaL_register(L, NULL, l);
#else
	luaL_setfuncs(L, l, 0);
#endif
}

INLINE int li_lua_equal(lua_State *L, int index1, int index2) {
#if LUA_VERSION_NUM == 501
	return lua_equal(L, index1, index2);
#else
	return lua_compare(L, index1, index2, LUA_OPEQ);
#endif
}

#if LUA_VERSION_NUM == 501
#define lua_rawlen(L, index) lua_objlen(L, index)
#endif

#endif

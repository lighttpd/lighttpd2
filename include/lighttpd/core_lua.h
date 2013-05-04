#ifndef _LIGHTTPD_CORE_LUA_H_
#define _LIGHTTPD_CORE_LUA_H_

#include <lighttpd/base.h>
#include <lua.h>

#define LI_LUA_REGISTRY_STATE   "lighttpd.state"
#define LI_LUA_REGISTRY_SERVER  "lighttpd.server"
#define LI_LUA_REGISTRY_WORKER  "lighttpd.worker"
#define LI_LUA_REGISTRY_GLOBALS "lighttpd.globals"

LI_API liLuaState *li_lua_state_get(lua_State *L);

INLINE void li_lua_lock(liLuaState *LL);
INLINE void li_lua_unlock(liLuaState *LL);

LI_API void li_lua_init_chunk_mt(lua_State *L);
LI_API liChunk* li_lua_get_chunk(lua_State *L, int ndx);
LI_API int li_lua_push_chunk(lua_State *L, liChunk *c);
LI_API liChunkQueue* li_lua_get_chunkqueue(lua_State *L, int ndx);
LI_API int li_lua_push_chunkqueue(lua_State *L, liChunkQueue *cq);

LI_API void li_lua_init_connection_mt(lua_State *L);
LI_API liConnection* li_lua_get_connection(lua_State *L, int ndx);
LI_API int li_lua_push_connection(lua_State *L, liConnection *con);

LI_API void li_lua_init_environment_mt(lua_State *L);
LI_API liEnvironment* li_lua_get_environment(lua_State *L, int ndx);
LI_API int li_lua_push_environment(lua_State *L, liEnvironment *env);

LI_API void li_lua_init_filter_mt(lua_State *L);
LI_API liFilter* li_lua_get_filter(lua_State *L, int ndx);
LI_API int li_lua_push_filter(lua_State *L, liFilter *f);
LI_API void li_lua_init_filters(lua_State *L, liServer* srv);
LI_API liFilter* li_lua_vrequest_add_filter_in(lua_State *L, liVRequest *vr, int state_ndx);
LI_API liFilter* li_lua_vrequest_add_filter_out(lua_State *L, liVRequest *vr, int state_ndx);

LI_API void li_lua_init_http_headers_mt(lua_State *L);
LI_API liHttpHeaders* li_lua_get_http_headers(lua_State *L, int ndx);
LI_API int li_lua_push_http_headers(lua_State *L, liHttpHeaders *headers);

LI_API void li_lua_init_physical_mt(lua_State *L);
LI_API liPhysical* li_lua_get_physical(lua_State *L, int ndx);
LI_API int li_lua_push_physical(lua_State *L, liPhysical *phys);

LI_API void li_lua_init_request_mt(lua_State *L);
LI_API liRequest* li_lua_get_request(lua_State *L, int ndx);
LI_API int li_lua_push_request(lua_State *L, liRequest *req);

LI_API liRequestUri* li_lua_get_requesturi(lua_State *L, int ndx);
LI_API int li_lua_push_requesturi(lua_State *L, liRequestUri *uri);

LI_API void li_lua_init_response_mt(lua_State *L);
LI_API liResponse* li_lua_get_response(lua_State *L, int ndx);
LI_API int li_lua_push_response(lua_State *L, liResponse *resp);

LI_API void li_lua_init_stat_mt(lua_State *L);
LI_API struct stat* li_lua_get_stat(lua_State *L, int ndx);
LI_API int li_lua_push_stat(lua_State *L, struct stat *st);

LI_API void li_lua_init_subrequest_mt(lua_State *L);

LI_API void li_lua_init_vrequest_mt(lua_State *L);
LI_API liVRequest* li_lua_get_vrequest(lua_State *L, int ndx);
LI_API int li_lua_push_vrequest(lua_State *L, liVRequest *vr);

LI_API void li_lua_init_coninfo_mt(lua_State *L);
LI_API liConInfo* li_lua_get_coninfo(lua_State *L, int ndx);
LI_API int li_lua_push_coninfo(lua_State *L, liConInfo *vr);


LI_API int li_lua_fixindex(lua_State *L, int ndx);

/* return 1 if value is found in mt (on top of the stack), 0 if it is not found (stack balance = 0)
 * table, key on stack at pos 0 and 1 (i.e. __index metho)
 */
LI_API int li_lua_metatable_index(lua_State *L);

LI_API void li_lua_init2(liLuaState* LL, liServer* srv, liWorker* wrk);

LI_API int li_lua_push_traceback(lua_State *L, int nargs);

/* nargs: number of arguments *with* object; object must be the first of the arguments
 * returns: FALSE: an error occured. stack balance -nargs (i.e. popped args)
 *          TRUE: stack balance nresp-narg; popped args, pushed results
 * srv/vr are only needed for error logging
 * if optional is TRUE don't log an error if the method doesn't exist
 */
LI_API gboolean li_lua_call_object(liServer *srv, liVRequest *vr, lua_State *L, const char* method, int nargs, int nresults, gboolean optional);

LI_API void li_lua_restore_globals(lua_State *L);
LI_API void li_lua_new_globals(lua_State *L);

/* joinWith " " (map tostring parameter[from..to]) */
LI_API GString* li_lua_print_get_string(lua_State *L, int from, int to);

/* pairs() for a GHashTable GString -> GString:
 *   Don't modify the hastable while iterating:
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
	assert(b);
}

INLINE void li_lua_unlock(liLuaState *LL) {
	g_static_rec_mutex_unlock(&LL->lualock);
}

#endif

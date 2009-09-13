
#include <lighttpd/core_lua.h>

#include <lualib.h>
#include <lauxlib.h>

#define LUA_CHUNK "liChunk*"
#define LUA_CHUNKQUEUE "liChunkQueue*"

static void init_chunk_mt(lua_State *L) {
	/* TODO */
}

static int lua_chunkqueue_add(lua_State *L) {
	liChunkQueue *cq;
	const char *s;
	size_t len;

	luaL_checkany(L, 2);
	cq = lua_get_chunkqueue(L, 1);
	if (cq == NULL) return 0;
	if (lua_isstring(L, 2)) {
		s = lua_tolstring(L, 2, &len);
		li_chunkqueue_append_mem(cq, s, len);
	} else {
		lua_pushliteral(L, "Wrong type for chunkqueue add");
		lua_error(L);
	}

	return 0;
}

static const luaL_Reg chunkqueue_mt[] = {
	{ "add", lua_chunkqueue_add },

	{ NULL, NULL }
};

static void init_chunkqueue_mt(lua_State *L) {
	luaL_register(L, NULL, chunkqueue_mt);

	lua_pushvalue(L, -1);
	lua_setfield(L, -2, "__index");
}

void lua_init_chunk_mt(lua_State *L) {
	if (luaL_newmetatable(L, LUA_CHUNK)) {
		init_chunk_mt(L);
	}
	lua_pop(L, 1);

	if (luaL_newmetatable(L, LUA_CHUNKQUEUE)) {
		init_chunkqueue_mt(L);
	}
	lua_pop(L, 1);
}

liChunk* lua_get_chunk(lua_State *L, int ndx) {
	if (!lua_isuserdata(L, ndx)) return NULL;
	if (!lua_getmetatable(L, ndx)) return NULL;
	luaL_getmetatable(L, LUA_CHUNK);
	if (lua_isnil(L, -1) || lua_isnil(L, -2) || !lua_equal(L, -1, -2)) {
		lua_pop(L, 2);
		return NULL;
	}
	lua_pop(L, 2);
	return *(liChunk**) lua_touserdata(L, ndx);
}

int lua_push_chunk(lua_State *L, liChunk *c) {
	liChunk **pc;

	pc = (liChunk**) lua_newuserdata(L, sizeof(liChunk*));
	*pc = c;

	if (luaL_newmetatable(L, LUA_CHUNK)) {
		init_chunk_mt(L);
	}

	lua_setmetatable(L, -2);
	return 1;
}

liChunkQueue* lua_get_chunkqueue(lua_State *L, int ndx) {
	if (!lua_isuserdata(L, ndx)) return NULL;
	if (!lua_getmetatable(L, ndx)) return NULL;
	luaL_getmetatable(L, LUA_CHUNKQUEUE);
	if (lua_isnil(L, -1) || lua_isnil(L, -2) || !lua_equal(L, -1, -2)) {
		lua_pop(L, 2);
		return NULL;
	}
	lua_pop(L, 2);
	return *(liChunkQueue**) lua_touserdata(L, ndx);
}

int lua_push_chunkqueue(lua_State *L, liChunkQueue *cq) {
	liChunkQueue **pcq;

	pcq = (liChunkQueue**) lua_newuserdata(L, sizeof(liChunkQueue*));
	*pcq = cq;

	if (luaL_newmetatable(L, LUA_CHUNKQUEUE)) {
		init_chunkqueue_mt(L);
	}

	lua_setmetatable(L, -2);
	return 1;
}

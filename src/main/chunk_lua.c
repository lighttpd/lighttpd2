
#include <lighttpd/core_lua.h>

#include <lualib.h>
#include <lauxlib.h>

#include <sys/stat.h>

#define LUA_CHUNK "liChunk*"
#define LUA_CHUNKQUEUE "liChunkQueue*"

static void init_chunk_mt(lua_State *L) {
	/* TODO */
}

typedef int (*lua_ChunkQueue_Attrib)(liChunkQueue *cq, lua_State *L);

static int lua_chunkqueue_attr_read_is_closed(liChunkQueue *cq, lua_State *L) {
	lua_pushboolean(L, cq->is_closed);
	return 1;
}

static int lua_chunkqueue_attr_write_is_closed(liChunkQueue *cq, lua_State *L) {
	cq->is_closed = lua_toboolean(L, 3);
	return 0;
}

#define AR(m) { #m, lua_chunkqueue_attr_read_##m, NULL }
#define AW(m) { #m, NULL, lua_chunkqueue_attr_write_##m }
#define ARW(m) { #m, lua_chunkqueue_attr_read_##m, lua_chunkqueue_attr_write_##m }

static const struct {
	const char* key;
	lua_ChunkQueue_Attrib read_attr, write_attr;
} chunkqueue_attribs[] = {
	ARW(is_closed),

	{ NULL, NULL, NULL }
};

static int lua_chunkqueue_index(lua_State *L) {
	liChunkQueue *cq;
	const char *key;
	int i;

	if (lua_gettop(L) != 2) {
		lua_pushstring(L, "incorrect number of arguments");
		lua_error(L);
	}

	if (li_lua_metatable_index(L)) return 1;

	cq = li_lua_get_chunkqueue(L, 1);
	if (!cq) return 0;

	if (lua_isnumber(L, 2)) return 0;
	if (!lua_isstring(L, 2)) return 0;

	key = lua_tostring(L, 2);
	for (i = 0; chunkqueue_attribs[i].key ; i++) {
		if (0 == strcmp(key, chunkqueue_attribs[i].key)) {
			if (chunkqueue_attribs[i].read_attr)
				return chunkqueue_attribs[i].read_attr(cq, L);
			break;
		}
	}

	lua_pushstring(L, "cannot read attribute ");
	lua_pushstring(L, key);
	lua_pushstring(L, " in chunkqueue");
	lua_concat(L, 3);
	lua_error(L);

	return 0;
}

static int lua_chunkqueue_newindex(lua_State *L) {
	liChunkQueue *cq;
	const char *key;
	int i;

	if (lua_gettop(L) != 3) {
		lua_pushstring(L, "incorrect number of arguments");
		lua_error(L);
	}

	cq = li_lua_get_chunkqueue(L, 1);
	if (!cq) return 0;

	if (lua_isnumber(L, 2)) return 0;
	if (!lua_isstring(L, 2)) return 0;

	key = lua_tostring(L, 2);
	for (i = 0; chunkqueue_attribs[i].key ; i++) {
		if (0 == strcmp(key, chunkqueue_attribs[i].key)) {
			if (chunkqueue_attribs[i].write_attr)
				return chunkqueue_attribs[i].write_attr(cq, L);
			break;
		}
	}

	lua_pushstring(L, "cannot write attribute ");
	lua_pushstring(L, key);
	lua_pushstring(L, "in chunkqueue");
	lua_concat(L, 3);
	lua_error(L);

	return 0;
}

static int lua_chunkqueue_add(lua_State *L) {
	liChunkQueue *cq;
	const char *s;
	size_t len;

	luaL_checkany(L, 2);
	cq = li_lua_get_chunkqueue(L, 1);
	if (cq == NULL) return 0;
	if (lua_isstring(L, 2)) {
		s = lua_tolstring(L, 2, &len);
		li_chunkqueue_append_mem(cq, s, len);
	} else if (lua_istable(L, 2)) {
		const char *filename = NULL;
		GString *g_filename = NULL;

		lua_getfield(L, -1, "filename");
		if (!lua_isnil(L, -1)) {
			filename = lua_tostring(L, -1);
			if (filename) g_filename = g_string_new(filename);
		}
		lua_pop(L, 1);

		if (g_filename) {
			struct stat st;

			if (-1 != stat(g_filename->str, &st) && S_ISREG(st.st_mode)) {
				li_chunkqueue_append_file(cq, g_filename, 0, st.st_size);
			}
		} else {
			goto fail;
		}
	} else {
		goto fail;
	}

	return 0;

fail:
	lua_pushliteral(L, "Wrong type for chunkqueue add");
	lua_error(L);

	return -1;
}

static int lua_chunkqueue_reset(lua_State *L) {
	liChunkQueue *cq;

	cq = li_lua_get_chunkqueue(L, 1);
	li_chunkqueue_reset(cq);

	return 0;
}

static const luaL_Reg chunkqueue_mt[] = {
	{ "__index", lua_chunkqueue_index },
	{ "__newindex", lua_chunkqueue_newindex },

	{ "add", lua_chunkqueue_add },
	{ "reset", lua_chunkqueue_reset },

	{ NULL, NULL }
};

static void init_chunkqueue_mt(lua_State *L) {
	luaL_register(L, NULL, chunkqueue_mt);

	lua_pushvalue(L, -1);
	lua_setfield(L, -2, "__index");
}

void li_lua_init_chunk_mt(lua_State *L) {
	if (luaL_newmetatable(L, LUA_CHUNK)) {
		init_chunk_mt(L);
	}
	lua_pop(L, 1);

	if (luaL_newmetatable(L, LUA_CHUNKQUEUE)) {
		init_chunkqueue_mt(L);
	}
	lua_pop(L, 1);
}

liChunk* li_lua_get_chunk(lua_State *L, int ndx) {
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

int li_lua_push_chunk(lua_State *L, liChunk *c) {
	liChunk **pc;

	pc = (liChunk**) lua_newuserdata(L, sizeof(liChunk*));
	*pc = c;

	if (luaL_newmetatable(L, LUA_CHUNK)) {
		init_chunk_mt(L);
	}

	lua_setmetatable(L, -2);
	return 1;
}

liChunkQueue* li_lua_get_chunkqueue(lua_State *L, int ndx) {
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

int li_lua_push_chunkqueue(lua_State *L, liChunkQueue *cq) {
	liChunkQueue **pcq;

	pcq = (liChunkQueue**) lua_newuserdata(L, sizeof(liChunkQueue*));
	*pcq = cq;

	if (luaL_newmetatable(L, LUA_CHUNKQUEUE)) {
		init_chunkqueue_mt(L);
	}

	lua_setmetatable(L, -2);
	return 1;
}

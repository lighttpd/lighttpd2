
#include <lighttpd/core_lua.h>

#include <sys/stat.h>

#define LUA_CHUNK "liChunk*"
#define LUA_CHUNKQUEUE "liChunkQueue*"

static HEDLEY_NEVER_INLINE void init_chunk_mt(lua_State *L) {
	/* TODO */
	UNUSED(L);
}

static void lua_push_chunk_metatable(lua_State *L) {
	if (li_lua_new_protected_metatable(L, LUA_CHUNK)) {
		init_chunk_mt(L);
	}
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

	if (!lua_isstring(L, 2)) {
		lua_pushliteral(L, "chunkqueue add expects simple string");
		lua_error(L);

		return -1;
	}

	s = lua_tolstring(L, 2, &len);
	li_chunkqueue_append_mem(cq, s, len);

	return 0;
}

static int _lua_chunkqueue_add_file(lua_State *L, gboolean tempfile) {
	liChunkQueue *cq;
	const char *filename;
	GString g_filename;
	size_t len;
	struct stat st;
	int fd, err;
	goffset start, length;

	luaL_checkany(L, 2);
	cq = li_lua_get_chunkqueue(L, 1);
	if (cq == NULL) return 0;
	if (!lua_isstring(L, 2)) {
		lua_pushliteral(L, "chunkqueue:add expects filename as first parameter");
		lua_error(L);

		return -1;
	}

	filename = lua_tolstring(L, 2, &len);
	g_filename = li_const_gstring(filename, len);
	if (LI_HANDLER_GO_ON != li_stat_cache_get_sync(NULL, &g_filename, &st, &err, &fd)) {
		lua_pushliteral(L, "chunkqueue:add couldn't open file: ");
		lua_pushvalue(L, 2);
		lua_concat(L, 2);
		lua_error(L);

		return -1;
	}

	start = 0;
	length = st.st_size;

	if (lua_gettop(L) >= 3) {
		if (!lua_isnumber(L, 3)) {
			lua_pushliteral(L, "chunkqueue:add expects number (or nothing) as second parameter");
			lua_error(L);

			close(fd);
			return -1;
		}

		start = lua_tonumber(L, 3);
	}
	if (lua_gettop(L) >= 4) {
		if (!lua_isnumber(L, 4)) {
			lua_pushliteral(L, "chunkqueue:add expects number (or nothing) as third parameter");
			lua_error(L);

			close(fd);
			return -1;
		}

		length = lua_tonumber(L, 3);
	}

	if (start < 0 || start >= st.st_size || length < 0 || start + length > st.st_size) {
		lua_pushliteral(L, "chunkqueue:add: Invalid start/length values");
		lua_error(L);

		close(fd);
		return -1;
	}

	if (tempfile) {
		li_chunkqueue_append_tempfile_fd(cq, g_string_new_len(filename, len), start, length, fd);
	} else {
		li_chunkqueue_append_file_fd(cq, NULL, start, length, fd);
	}

	return 0;
}

static int lua_chunkqueue_add_file(lua_State *L) {
	return _lua_chunkqueue_add_file(L, FALSE);
}

static int lua_chunkqueue_add_temp_file(lua_State *L) {
	return _lua_chunkqueue_add_file(L, TRUE);
}

static int lua_chunkqueue_reset(lua_State *L) {
	liChunkQueue *cq;

	cq = li_lua_get_chunkqueue(L, 1);
	li_chunkqueue_reset(cq);

	return 0;
}

static int lua_chunkqueue_steal_all(lua_State *L) {
	liChunkQueue *cq, *cq_from;

	cq = li_lua_get_chunkqueue(L, 1);
	cq_from = li_lua_get_chunkqueue(L, 2);
	if (!cq_from) {
		lua_pushliteral(L, "Expected source chunkqueue to steal from");
		return lua_error(L);
	}

	li_chunkqueue_steal_all(cq, cq_from);

	return 0;
}

static int lua_chunkqueue_skip_all(lua_State *L) {
	liChunkQueue *cq;

	cq = li_lua_get_chunkqueue(L, 1);
	li_chunkqueue_skip_all(cq);

	return 0;
}


static const luaL_Reg chunkqueue_mt[] = {
	{ "__index", lua_chunkqueue_index },
	{ "__newindex", lua_chunkqueue_newindex },

	{ "add", lua_chunkqueue_add },
	{ "add_file", lua_chunkqueue_add_file },
	{ "add_temp_file", lua_chunkqueue_add_temp_file },
	{ "reset", lua_chunkqueue_reset },
	{ "steal_all", lua_chunkqueue_steal_all },
	{ "skip_all", lua_chunkqueue_skip_all },

	{ NULL, NULL }
};

static HEDLEY_NEVER_INLINE void init_chunkqueue_mt(lua_State *L) {
	luaL_register(L, NULL, chunkqueue_mt);
}

static void lua_push_chunkqueue_metatable(lua_State *L) {
	if (li_lua_new_protected_metatable(L, LUA_CHUNKQUEUE)) {
		init_chunkqueue_mt(L);
	}
}

void li_lua_init_chunk_mt(lua_State *L) {
	lua_push_chunk_metatable(L);
	lua_pop(L, 1);

	lua_push_chunkqueue_metatable(L);
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

	if (NULL == c) {
		lua_pushnil(L);
		return 1;
	}

	pc = (liChunk**) lua_newuserdata(L, sizeof(liChunk*));
	*pc = c;

	lua_push_chunk_metatable(L);
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

	if (NULL == cq) {
		lua_pushnil(L);
		return 1;
	}

	pcq = (liChunkQueue**) lua_newuserdata(L, sizeof(liChunkQueue*));
	*pcq = cq;

	lua_push_chunkqueue_metatable(L);
	lua_setmetatable(L, -2);
	return 1;
}

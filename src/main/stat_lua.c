
#include <lighttpd/core_lua.h>

/* struct stat */
#define LUA_STAT "struct stat"

struct stat* li_lua_get_stat(lua_State *L, int ndx) {
	if (!lua_isuserdata(L, ndx)) return NULL;
	if (!lua_getmetatable(L, ndx)) return NULL;
	luaL_getmetatable(L, LUA_STAT);
	if (lua_isnil(L, -1) || lua_isnil(L, -2) || !li_lua_equal(L, -1, -2)) {
		lua_pop(L, 2);
		return NULL;
	}
	lua_pop(L, 2);
	return (struct stat*) lua_touserdata(L, ndx);
}

typedef int (*lua_stat_Attrib)(struct stat *st, lua_State *L);

static int lua_stat_attr_read_is_file(struct stat *st, lua_State *L) {
	lua_pushboolean(L, S_ISREG(st->st_mode));
	return 1;
}

static int lua_stat_attr_read_is_dir(struct stat *st, lua_State *L) {
	lua_pushboolean(L, S_ISDIR(st->st_mode));
	return 1;
}

static int lua_stat_attr_read_is_char(struct stat *st, lua_State *L) {
	lua_pushboolean(L, S_ISCHR(st->st_mode));
	return 1;
}

static int lua_stat_attr_read_is_block(struct stat *st, lua_State *L) {
	lua_pushboolean(L, S_ISBLK(st->st_mode));
	return 1;
}

static int lua_stat_attr_read_is_socket(struct stat *st, lua_State *L) {
	lua_pushboolean(L, S_ISSOCK(st->st_mode));
	return 1;
}

static int lua_stat_attr_read_is_link(struct stat *st, lua_State *L) {
	lua_pushboolean(L, S_ISLNK(st->st_mode));
	return 1;
}

static int lua_stat_attr_read_is_fifo(struct stat *st, lua_State *L) {
	lua_pushboolean(L, S_ISFIFO(st->st_mode));
	return 1;
}

static int lua_stat_attr_read_mode(struct stat *st, lua_State *L) {
	lua_pushinteger(L, st->st_mode);
	return 1;
}

static int lua_stat_attr_read_mtime(struct stat *st, lua_State *L) {
	lua_pushinteger(L, st->st_mtime);
	return 1;
}

static int lua_stat_attr_read_ctime(struct stat *st, lua_State *L) {
	lua_pushinteger(L, st->st_ctime);
	return 1;
}

static int lua_stat_attr_read_atime(struct stat *st, lua_State *L) {
	lua_pushinteger(L, st->st_atime);
	return 1;
}

static int lua_stat_attr_read_uid(struct stat *st, lua_State *L) {
	lua_pushinteger(L, st->st_uid);
	return 1;
}

static int lua_stat_attr_read_gid(struct stat *st, lua_State *L) {
	lua_pushinteger(L, st->st_gid);
	return 1;
}

static int lua_stat_attr_read_size(struct stat *st, lua_State *L) {
	lua_pushinteger(L, st->st_size);
	return 1;
}

static int lua_stat_attr_read_ino(struct stat *st, lua_State *L) {
	lua_pushinteger(L, st->st_ino);
	return 1;
}

static int lua_stat_attr_read_dev(struct stat *st, lua_State *L) {
	lua_pushinteger(L, st->st_dev);
	return 1;
}

#define AR(m) { #m, lua_stat_attr_read_##m, NULL }
#define AW(m) { #m, NULL, lua_stat_attr_write_##m }
#define ARW(m) { #m, lua_stat_attr_read_##m, lua_stat_attr_write_##m }

static const struct {
	const char* key;
	lua_stat_Attrib read_attr, write_attr;
} stat_attribs[] = {
	AR(is_file),
	AR(is_dir),
	AR(is_char),
	AR(is_block),
	AR(is_socket),
	AR(is_link),
	AR(is_fifo),
	AR(mode),
	AR(mtime),
	{ "ctime", lua_stat_attr_read_ctime, NULL }, /* avoid poisoned ctime */
	AR(atime),
	AR(uid),
	AR(gid),
	AR(size),
	AR(ino),
	AR(dev),

	{ NULL, NULL, NULL }
};

static int lua_stat_index(lua_State *L) {
	struct stat *st;
	const char *key;
	int i;

	if (lua_gettop(L) != 2) {
		lua_pushstring(L, "incorrect number of arguments");
		lua_error(L);
	}

	if (li_lua_metatable_index(L)) return 1;

	st = li_lua_get_stat(L, 1);
	if (!st) return 0;

	if (lua_isnumber(L, 2)) return 0;
	if (!lua_isstring(L, 2)) return 0;

	key = lua_tostring(L, 2);
	for (i = 0; stat_attribs[i].key ; i++) {
		if (0 == strcmp(key, stat_attribs[i].key)) {
			if (stat_attribs[i].read_attr)
				return stat_attribs[i].read_attr(st, L);
			break;
		}
	}

	lua_pushstring(L, "cannot read attribute ");
	lua_pushstring(L, key);
	lua_pushstring(L, " in struct stat");
	lua_concat(L, 3);
	lua_error(L);

	return 0;
}

static int lua_stat_newindex(lua_State *L) {
	struct stat *st;
	const char *key;
	int i;

	if (lua_gettop(L) != 3) {
		lua_pushstring(L, "incorrect number of arguments");
		lua_error(L);
	}

	st = li_lua_get_stat(L, 1);
	if (!st) return 0;

	if (lua_isnumber(L, 2)) return 0;
	if (!lua_isstring(L, 2)) return 0;

	key = lua_tostring(L, 2);
	for (i = 0; stat_attribs[i].key ; i++) {
		if (0 == strcmp(key, stat_attribs[i].key)) {
			if (stat_attribs[i].write_attr)
				return stat_attribs[i].write_attr(st, L);
			break;
		}
	}

	lua_pushstring(L, "cannot write attribute ");
	lua_pushstring(L, key);
	lua_pushstring(L, "in struct stat");
	lua_concat(L, 3);
	lua_error(L);

	return 0;
}

static const luaL_Reg stat_mt[] = {
	{ "__index", lua_stat_index },
	{ "__newindex", lua_stat_newindex },
	{ NULL, NULL }
};

static HEDLEY_NEVER_INLINE void init_stat_mt(lua_State *L) {
	li_lua_setfuncs(L, stat_mt);
}

static void lua_push_stat_metatable(lua_State *L) {
	if (li_lua_new_protected_metatable(L, LUA_STAT)) {
		init_stat_mt(L);
	}
}

void li_lua_init_stat_mt(lua_State *L) {
	lua_push_stat_metatable(L);
	lua_pop(L, 1);
}

int li_lua_push_stat(lua_State *L, struct stat *st) {
	struct stat *pst;

	if (NULL == st) {
		lua_pushnil(L);
		return 1;
	}

	pst = (struct stat*) lua_newuserdata(L, sizeof(struct stat));
	*pst = *st;

	lua_push_stat_metatable(L);
	lua_setmetatable(L, -2);
	return 1;
}

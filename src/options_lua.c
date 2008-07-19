
#include "options_lua.h"
#include "log.h"

/* replace a negative stack index with a positive one,
 * so that you don't need to care about push/pop
 */
static int lua_fixindex(lua_State *L, int ndx) {
	int top;
	if (ndx < 0 && ndx >= -(top = lua_gettop(L)))
		ndx = top + ndx + 1;
	return ndx;
}

static option* option_from_lua_table(server *srv, lua_State *L, int ndx) {
	option *opt = NULL, *sub_option;
	GArray *list = NULL;
	GHashTable *hash = NULL;
	int ikey;
	GString *skey;

	ndx = lua_fixindex(L, ndx);
	lua_pushnil(L);
	while (lua_next(L, ndx) != 0) {
		switch (lua_type(L, -2)) {
		case LUA_TNUMBER:
			if (hash) goto mixerror;
			if (!list) {
				opt = option_new_list();
				list = opt->value.opt_list;
			}
			ikey = lua_tointeger(L, -2);
			if (ikey < 0) {
				ERROR(srv, "Invalid key < 0: %i - skipping entry", ikey);
				lua_pop(L, 1);
				continue;
			}
			sub_option = option_from_lua(L);
			if (!sub_option) continue;
			if ((size_t) ikey >= list->len) {
				g_array_set_size(list, ikey + 1);
			}
			g_array_index(list, option*, ikey) = sub_option;
			break;

		case LUA_TSTRING:
			if (list) goto mixerror;
			if (!hash) {
				opt = option_new_hash();
				hash = opt->value.opt_hash;
			}
			skey = lua_togstring(L, -2);
			if (g_hash_table_lookup(hash, skey)) {
				ERROR(srv, "Key already exists in hash: '%s' - skipping entry", skey->str);
				lua_pop(L, 1);
				continue;
			}
			sub_option = option_from_lua(L);
			if (!sub_option) {
				g_string_free(skey, TRUE);
				continue;
			}
			g_hash_table_insert(hash, skey, sub_option);
			break;

		default:
			ERROR(srv, "Unexpted key type in table: %s (%i) - skipping entry", lua_typename(L, -1), lua_type(L, -1));
			lua_pop(L, 1);
			break;
		}
	}

	return opt;

mixerror:
	ERROR(srv, "%s", "Cannot mix list with hash; skipping remaining part of table");
	lua_pop(L, 2);
	return opt;
}


option* option_from_lua(server *srv, lua_State *L) {
	option *opt;

	switch (lua_type(L, -1)) {
	case LUA_TNIL:
		lua_pop(L, 1);
		return NULL;

	case LUA_TBOOLEAN:
		opt = option_new_bool(lua_toboolean(L, -1));
		lua_pop(L, 1);
		return opt;

	case LUA_TNUMBER:
		opt = option_new_int(lua_tointeger(L, -1));
		lua_pop(L, 1);
		return opt;

	case LUA_TSTRING:
		opt = option_new_string(lua_togstring(L, -1));
		lua_pop(L, 1);
		return opt;

	case LUA_TTABLE:
		opt = option_from_lua_table(srv, L, -1);
		lua_pop(L, 1);
		return opt;

	case LUA_TLIGHTUSERDATA:
	case LUA_TFUNCTION:
	case LUA_TUSERDATA:
	case LUA_TTHREAD:
	case LUA_TNONE:
	default:
		ERROR(srv, "Unexpected lua type: %s (%i)", lua_typename(L, -1), lua_type(L, -1));
		lua_pop(L, 1);
		return NULL;
	}
}

GString* lua_togstring(lua_State *L, int ndx) {
	const char *buf;
	size_t len = 0;
	GString *str = NULL;

	if (lua_type(L, ndx) == LUA_TSTRING) {
		buf = lua_tolstring(L, ndx, &len);
		if (buf) str = g_string_new_len(buf, len);
	} else {
		lua_pushvalue(L, ndx);
		buf = lua_tolstring(L, -1, &len);
		if (buf) str = g_string_new_len(buf, len);
		lua_pop(L, 1);
	}

	return str;
}

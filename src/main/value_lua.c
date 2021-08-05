
#include <lighttpd/value_lua.h>
#include <lighttpd/condition_lua.h>
#include <lighttpd/actions_lua.h>
#include <lighttpd/core_lua.h>

#include <lauxlib.h>

#define LUA_KVLIST_VALUE "li KeyValue list (string, liValue*)"

static int lua_kvlist_index(lua_State *L) {
	guint len, i;
	gboolean nil_key;

	switch (lua_type(L, 2)) {
	case LUA_TNUMBER:
		lua_rawget(L, 1);
		return 1;
	case LUA_TSTRING:
		nil_key = FALSE;
		break;
	case LUA_TNIL:
		nil_key = TRUE;
		break;
	default:
		goto fail;
	}

	if (LUA_TTABLE != lua_type(L, 1)) goto fail;

	len = lua_objlen(L, 1);
	for (i = len; i >= 1; lua_pop(L, 1), --i) {
		lua_rawgeti(L, 1, i);

		if (LUA_TTABLE != lua_type(L, -1)) continue;
		if (2 != lua_objlen(L, -1)) continue;

		lua_rawgeti(L, -1, 1);
		switch (lua_type(L, -1)) {
		case LUA_TSTRING:
			if (nil_key) break;
			if (!lua_equal(L, -1, 2)) break;
			lua_rawgeti(L, -2, 2);
			return 1;
		case LUA_TNIL:
			if (!nil_key) break;
			lua_rawgeti(L, -2, 2);
			return 1;
		default:
			break;
		}
		lua_pop(L, 1);
	}

fail:
	lua_pushnil(L);
	return 1;
}

static HEDLEY_NEVER_INLINE void init_kvlist_mt(lua_State *L) {
	lua_pushcclosure(L, lua_kvlist_index, 0);
	lua_setfield(L, -2, "__index");
}

static void lua_push_kvlist_metatable(lua_State *L) {
	if (luaL_newmetatable(L, LUA_KVLIST_VALUE)) {
		init_kvlist_mt(L);
	}
}


static liValue* li_value_from_lua_table(liServer *srv, lua_State *L, int ndx) {
	liValue *val, *entry;
	gboolean is_list = FALSE, is_hash = FALSE;
	int ikey;
	liValue *kv_key, *kv_pair;

	val = li_value_new_list();

	ndx = li_lua_fixindex(L, ndx);
	lua_pushnil(L);
	while (lua_next(L, ndx) != 0) {
		switch (lua_type(L, -2)) {
		case LUA_TNUMBER:
			if (is_hash) goto mixerror;
			is_list = TRUE;
			ikey = lua_tointeger(L, -2) - 1;
			if (ikey < 0) {
				ERROR(srv, "Invalid key < 0: %i - skipping entry", ikey + 1);
				lua_pop(L, 1);
				continue;
			}
			entry = li_value_from_lua(srv, L);
			if (NULL == entry) continue;
			li_value_list_set(val, ikey, entry);
			break;

		case LUA_TSTRING:
			if (is_list) goto mixerror;
			is_hash = TRUE;
			kv_key = li_value_new_string(li_lua_togstring(L, -2));
			entry = li_value_from_lua(srv, L);
			if (NULL == entry) {
				li_value_free(kv_key);
				continue;
			}
			kv_pair = li_value_new_list();
			li_value_list_append(kv_pair, kv_key);
			li_value_list_append(kv_pair, entry);
			li_value_list_append(val, kv_pair);
			break;

		default:
			ERROR(srv, "Unexpected key type in table: %s (%i) - skipping entry", lua_typename(L, lua_type(L, -1)), lua_type(L, -1));
			lua_pop(L, 1);
			break;
		}
	}

	return val;

mixerror:
	ERROR(srv, "%s", "Cannot mix list with hash; skipping remaining part of table");
	lua_pop(L, 2);
	return val;
}


liValue* li_value_from_lua(liServer *srv, lua_State *L) {
	liValue *val;

	switch (lua_type(L, -1)) {
	case LUA_TNIL:
		lua_pop(L, 1);
		return NULL;

	case LUA_TBOOLEAN:
		val = li_value_new_bool(lua_toboolean(L, -1));
		lua_pop(L, 1);
		return val;

	case LUA_TNUMBER:
		val = li_value_new_number(lua_tonumber(L, -1));
		lua_pop(L, 1);
		return val;

	case LUA_TSTRING:
		val = li_value_new_string(li_lua_togstring(L, -1));
		lua_pop(L, 1);
		return val;

	case LUA_TTABLE:
		val = li_value_from_lua_table(srv, L, -1);
		lua_pop(L, 1);
		return val;

	case LUA_TUSERDATA:
		{ /* check for action */
			liAction *a = li_lua_get_action(L, -1);
			if (a) {
				li_action_acquire(a);
				lua_pop(L, 1);
				return li_value_new_action(srv, a);
			}
		}
		{ /* check for condition */
			liCondition *c = li_lua_get_condition(L, -1);
			if (c) {
				li_condition_acquire(c);
				lua_pop(L, 1);
				return li_value_new_condition(srv, c);
			}
		}
		ERROR(srv, "%s", "Unknown lua userdata");
		lua_pop(L, 1);
		return NULL;

	case LUA_TFUNCTION: {
			liAction *a = li_lua_make_action(L, -1);
			lua_pop(L, 1);
			return li_value_new_action(srv, a);
		}

	case LUA_TLIGHTUSERDATA:
	case LUA_TTHREAD:
	case LUA_TNONE:
	default:
		ERROR(srv, "Unexpected lua type: %s (%i)", lua_typename(L, lua_type(L, -1)), lua_type(L, -1));
		lua_pop(L, 1);
		return NULL;
	}
}

GString* li_lua_togstring(lua_State *L, int ndx) {
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

int li_lua_push_value(lua_State *L, liValue *value) {
	if (NULL == value) {
		lua_pushnil(L);
		return 1;
	}

	switch (value->type) {
	case LI_VALUE_NONE:
		lua_pushnil(L);
		break;
	case LI_VALUE_BOOLEAN:
		lua_pushboolean(L, value->data.boolean);
		break;
	case LI_VALUE_NUMBER:
		lua_pushinteger(L, value->data.number);
		break;
	case LI_VALUE_STRING:
		lua_pushlstring(L, GSTR_LEN(value->data.string));
		break;
	case LI_VALUE_LIST: {
		lua_newtable(L);
		LI_VALUE_FOREACH(entry, value)
			li_lua_push_value(L, entry);
			lua_rawseti(L, -2, _entry_i + 1);
		LI_VALUE_END_FOREACH()
		/* kvlist lookup for string/nil keys */
		lua_push_kvlist_metatable(L);
		lua_setmetatable(L, -2);
	} break;
	case LI_VALUE_ACTION:
		li_action_acquire(value->data.val_action.action);
		li_lua_push_action(value->data.val_action.srv, L, value->data.val_action.action);
		break;
	case LI_VALUE_CONDITION:
		li_condition_acquire(value->data.val_cond.cond);
		li_lua_push_condition(value->data.val_cond.srv, L, value->data.val_cond.cond);
		break;
	default: /* ignore error and push nil */
		lua_pushnil(L);
		break;
	}
	return 1;
}

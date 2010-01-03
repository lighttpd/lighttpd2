/*
 * mod_lua - include lua actions
 *
 * Description:
 *     mod_lua
 *
 * Setups:
 *     none
 * Options:
 *     none
 * Actions:
 *     lua.handler filename, [ "ttl": 300 ]
 *         - Basically the same as include_lua (no setup.* calls allowed), but loads the script
 *           in a worker specific lua_State, so it doesn't use the server wide lua lock.
 *         - You can give a ttl, after which the file is checked for modifications
 *           and reloaded. The default value 0 disables reloading.
 *
 * Example config:
 *     lua.handler "/etc/lighttpd/pathrewrite.lua";
 *
 * Todo:
 *     - Support lua plugins (new action/setup callbacks, plugin hooks)
 *
 * Author:
 *     Copyright (c) 2009 Stefan Bühler
 * License:
 *     MIT, see COPYING file in the lighttpd 2 tree
 */

#include <lighttpd/base.h>

#include <lighttpd/core_lua.h>
#include <lighttpd/config_lua.h>
#include <lighttpd/condition_lua.h>
#include <lighttpd/value_lua.h>
#include <lighttpd/actions_lua.h>

#include <lualib.h>
#include <lauxlib.h>

LI_API gboolean mod_lua_init(liModules *mods, liModule *mod);
LI_API gboolean mod_lua_free(liModules *mods, liModule *mod);

typedef struct module_config module_config;
struct module_config {
	liPlugin *main_plugin;
	GPtrArray *lua_plugins;
};

typedef struct lua_worker_config lua_worker_config;
struct lua_worker_config {
	liAction *act;
	time_t ts_loaded;
};

typedef struct lua_config lua_config;
struct lua_config {
	GString *filename;
	guint ttl;

	gint initialized; /* 0: not initialized, 1: initialized, 2: initializing */
	lua_worker_config *worker_config;
};

static void lua_config_check_init(liServer *srv, lua_config *conf) {
	gint i;
	while (1 != (i = g_atomic_int_get(&conf->initialized))) {
		if (i != 0) {
			ev_sleep(0.01);
		} else if (g_atomic_int_compare_and_exchange(&conf->initialized, 0, 2)) {
			conf->worker_config = g_slice_alloc0(sizeof(lua_worker_config) * srv->worker_count);
			g_atomic_int_set(&conf->initialized, 1);
			return;
		}
	}
}

static liHandlerResult lua_handle(liVRequest *vr, gpointer param, gpointer *context) {
	lua_config *conf = (lua_config*) param;
	lua_worker_config *wc;
	gboolean timeout = FALSE;
	liHandlerResult res;
	UNUSED(context);

	lua_config_check_init(vr->wrk->srv, conf);

	wc = &conf->worker_config[vr->wrk->ndx];

	if (wc->act) timeout = (conf->ttl > 0 && wc->ts_loaded + conf->ttl >= CUR_TS(vr->wrk));

	if (!wc->act || timeout) {
		int err;
		struct stat st;

		res = li_stat_cache_get(vr, conf->filename, &st, &err, NULL);
		switch (res) {
		case LI_HANDLER_ERROR:
			VR_ERROR(vr, "lua.handler: couldn't stat file '%s': %s", conf->filename->str, g_strerror(err));
			return LI_HANDLER_ERROR;
		case LI_HANDLER_WAIT_FOR_EVENT:
			return LI_HANDLER_WAIT_FOR_EVENT;
		default:
			break;
		}

		if (timeout && st.st_mtime <= wc->ts_loaded) {
			wc->ts_loaded = CUR_TS(vr->wrk);
			goto loaded;
		}

		li_action_release(vr->wrk->srv, wc->act);
		wc->act = NULL;
		if (!li_config_lua_load(vr->wrk->L, vr->wrk->srv, conf->filename->str, &wc->act, FALSE) || !wc->act) {
			VR_ERROR(vr, "lua.handler: couldn't load '%s'", conf->filename->str);
			return LI_HANDLER_ERROR;
		}
	}

loaded:
	li_action_enter(vr, wc->act);

	return LI_HANDLER_GO_ON;
}


static void lua_config_free(liServer *srv, gpointer param) {
	lua_config *conf = (lua_config*) param;
	UNUSED(srv);

	if (conf->worker_config) {
		lua_worker_config *wc = conf->worker_config;
		guint i;
		for (i = 0; i < srv->worker_count; i++) {
			li_action_release(srv, wc[i].act);
		}
		g_slice_free1(sizeof(lua_worker_config) * srv->worker_count, wc);
	}
	g_string_free(conf->filename, TRUE);

	g_slice_free(lua_config, conf);
}

static lua_config* lua_config_new(GString *filename, guint ttl) {
	lua_config *conf = g_slice_new0(lua_config);
	conf->filename = filename;
	conf->ttl = ttl;

	return conf;
}

static const GString /* lua option names */
	lon_ttl = { CONST_STR_LEN("ttl"), 0 }
;

static liAction* lua_handler_create(liServer *srv, liPlugin* p, liValue *val, gpointer userdata) {
	liValue *v_filename = NULL, *v_options = NULL;
	lua_config *conf;
	guint ttl = 0;
	UNUSED(p); UNUSED(userdata);

	if (val) {
		if (val->type == LI_VALUE_STRING) {
			v_filename = val;
		} else if (val->type == LI_VALUE_LIST) {
			GArray *l = val->data.list;
			if (l->len > 0) v_filename = g_array_index(l, liValue*, 0);
			if (l->len > 1) v_options = g_array_index(l, liValue*, 1);
		}
	}

	if (v_filename && v_filename->type != LI_VALUE_STRING) {
		v_filename = NULL;
	}

	if (!v_filename) {
		ERROR(srv, "%s", "lua.handler expects at least a filename, or a filename and some options");
		return NULL;
	}
	if (v_options && v_options->type != LI_VALUE_HASH) {
		ERROR(srv, "%s", "lua.handler expects options in a hash");
		return NULL;
	}

	if (v_options) {
		GHashTable *ht = v_options->data.hash;
		GHashTableIter it;
		gpointer pkey, pvalue;

		g_hash_table_iter_init(&it, ht);
		while (g_hash_table_iter_next(&it, &pkey, &pvalue)) {
			GString *key = pkey;
			liValue *value = pvalue;

			if (g_string_equal(key, &lon_ttl)) {
				if (value->type != LI_VALUE_NUMBER || value->data.number <= 0) {
					ERROR(srv, "lua.handler option '%s' expects positive integer as parameter", lon_ttl.str);
					goto option_failed;
				}
				ttl = value->data.number;
			} else {
				ERROR(srv, "unknown option for lua.handler '%s'", key->str);
				goto option_failed;
			}
		}
	}

	conf = lua_config_new(li_value_extract_ptr(v_filename), ttl);

	return li_action_new_function(lua_handle, NULL, lua_config_free, conf);

option_failed:
	return NULL;
}

/* lua plugins */

typedef struct luaPlugin luaPlugin;
struct luaPlugin {
	GArray *actions, *setups;

	GString *filename;
};

static int push_args(lua_State *L, liValue *val) {
	if (val->type == LI_VALUE_LIST) {
		GArray *list = val->data.list;
		guint i;
		for (i = 0; i < list->len; i++) {
			liValue *subval = g_array_index(list, liValue*, i);
			li_lua_push_value(L, subval);
		}
		return list->len;
	} else {
		return li_lua_push_value(L, val);
	}
}

static gboolean lua_plugin_handle_setup(liServer *srv, liPlugin *p, liValue *val, gpointer userdata) {
	lua_State *L = srv->L;
	int lua_ref = GPOINTER_TO_INT(userdata);
	int nargs, errfunc;
	gboolean res;

	UNUSED(p);

	li_lua_lock(srv);

	lua_rawgeti(L, LUA_REGISTRYINDEX, lua_ref);
	nargs = push_args(L, val);

	errfunc = li_lua_push_traceback(L, 0);
	if (lua_pcall(L, nargs, 1, errfunc)) {
		ERROR(srv, "lua_pcall(): %s", lua_tostring(L, -1));
		lua_pop(L, 1);
		res = FALSE;
	} else {
		res = TRUE;
		/* accept nil and true; everything else is "false" */
		if (!lua_isnil(L, -1) && (!lua_isboolean(L, -1) || !lua_toboolean(L, -1))) {
			res = FALSE;
		}
		lua_pop(L, 1);
	}
	lua_remove(L, errfunc);

	li_lua_unlock(srv);

	return res;
}

static liAction* lua_plugin_handle_action(liServer *srv, liPlugin* p, liValue *val, gpointer userdata) {
	lua_State *L = srv->L;
	int lua_ref = GPOINTER_TO_INT(userdata);
	int nargs, errfunc;
	liAction *res = NULL;

	UNUSED(p);

	li_lua_lock(srv);

	lua_rawgeti(L, LUA_REGISTRYINDEX, lua_ref);
	nargs = push_args(L, val);

	errfunc = li_lua_push_traceback(L, nargs);
	if (lua_pcall(L, nargs, 1, errfunc)) {
		ERROR(srv, "lua_pcall(): %s", lua_tostring(L, -1));
		lua_pop(L, 1);
	} else {
		res = li_lua_get_action_ref(L, -1);
		if (res == NULL) {
			ERROR(srv, "%s", "lua plugin action-create callback didn't return an action");
		}
		lua_pop(L, 1);
	}
	lua_remove(L, errfunc);

	li_lua_unlock(srv);

	return res;
}

static void lua_plugin_free_data(liServer *srv, luaPlugin *lp) {
	lua_State *L = srv->L;
	guint i;

	if (L) li_lua_lock(srv);

	for (i = 0; i < lp->actions->len; i++) {
		liPluginAction *pa = &g_array_index(lp->actions, liPluginAction, i);
		int lua_ref = GPOINTER_TO_INT(pa->userdata);
		if (L) luaL_unref(L, LUA_REGISTRYINDEX, lua_ref);
		g_free((gchar*) pa->name);
	}
	g_array_free(lp->actions, TRUE);
	for (i = 0; i < lp->setups->len; i++) {
		liPluginSetup *ps = &g_array_index(lp->setups, liPluginSetup, i);
		int lua_ref = GPOINTER_TO_INT(ps->userdata);
		if (L) luaL_unref(L, LUA_REGISTRYINDEX, lua_ref);
		g_free((gchar*) ps->name);
	}
	g_array_free(lp->setups, TRUE);

	if (L) li_lua_unlock(srv);

	if (lp->filename)
		g_string_free(lp->filename, TRUE);

	g_slice_free(luaPlugin, lp);
}

static luaPlugin* lua_plugin_create_data(liServer *srv, lua_State *L) {
	luaPlugin *lp;

	lp = g_slice_new0(luaPlugin);
	lp->actions = g_array_new(TRUE, TRUE, sizeof(liPluginAction));
	lp->setups = g_array_new(TRUE, TRUE, sizeof(liPluginSetup));

	lua_getfield(L, LUA_GLOBALSINDEX, "actions");
	if (lua_istable(L, -1)) {
		liPluginAction plug_action;
		int ndx;

		plug_action.create_action = lua_plugin_handle_action;

		ndx = li_lua_fixindex(L, -1);
		lua_pushnil(L);
		while (lua_next(L, ndx) != 0) {
			switch (lua_type(L, -2)) {
			case LUA_TSTRING:
				plug_action.name = g_strdup(lua_tostring(L, -2));
				plug_action.userdata = GINT_TO_POINTER(luaL_ref(L, LUA_REGISTRYINDEX));
				g_array_append_val(lp->actions, plug_action);
				break;

			default:
				ERROR(srv, "Unexpted key type in table 'actions': %s (%i) - skipping entry", lua_typename(L, -1), lua_type(L, -1));
				lua_pop(L, 1);
				break;
			}
		}
	}
	lua_pop(L, 1);

	lua_getfield(L, LUA_GLOBALSINDEX, "setups");
	if (lua_istable(L, -1)) {
		liPluginSetup plug_setup;
		int ndx;

		plug_setup.setup = lua_plugin_handle_setup;

		ndx = li_lua_fixindex(L, -1);
		lua_pushnil(L);
		while (lua_next(L, ndx) != 0) {
			switch (lua_type(L, -2)) {
			case LUA_TSTRING:
				plug_setup.name = g_strdup(lua_tostring(L, -2));
				plug_setup.userdata = GINT_TO_POINTER(luaL_ref(L, LUA_REGISTRYINDEX));
				g_array_append_val(lp->setups, plug_setup);
				break;

			default:
				ERROR(srv, "Unexpted key type in table 'setups': %s (%i) - skipping entry", lua_typename(L, -1), lua_type(L, -1));
				lua_pop(L, 1);
				break;
			}
		}
	}
	lua_pop(L, 1);

	return lp;
}

static const liPluginOption lp_options[] = {
	{ NULL, 0, NULL, NULL, NULL }
};

static void lua_plugin_free(liServer *srv, liPlugin *p) {
	luaPlugin *lp = p->data;

	lua_plugin_free_data(srv, lp);
}

static void lua_plugin_init(liServer *srv, liPlugin *p, gpointer userdata) {
	luaPlugin *lp = userdata;
	UNUSED(srv);

	p->options = lp_options;
	p->actions = &g_array_index(lp->actions, liPluginAction, 0);
	p->setups = &g_array_index(lp->setups, liPluginSetup, 0);;

	p->data = lp;
	p->free = lua_plugin_free;
}

static gboolean lua_plugin_load(liServer *srv, liPlugin *p, GString *filename) {
	int errfunc;
	int lua_stack_top;
	lua_State *L = srv->L;
	luaPlugin *lp;
	module_config *mc = p->data;
	liPlugin *newp;

	li_lua_lock(srv);

	lua_stack_top = lua_gettop(L);

	li_lua_new_globals(L);

	if (0 != luaL_loadfile(L, filename->str)) {
		ERROR(srv, "Loading lua plugin '%s' failed: %s", filename->str, lua_tostring(L, -1));
		goto failed_unlock_lua;
	}

	DEBUG(srv, "Loaded lua plugin '%s'", filename->str);

	li_lua_config_publish_str_hash(srv, L, srv->setups, li_lua_config_handle_server_setup);
	lua_setfield(L, LUA_GLOBALSINDEX, "setup");

	li_lua_config_publish_str_hash(srv, L, srv->actions, li_lua_config_handle_server_action);
	lua_setfield(L, LUA_GLOBALSINDEX, "action");

	li_lua_push_lvalues_dict(srv, L);

	lua_pushvalue(L, LUA_GLOBALSINDEX);
	lua_setfenv(L, -2);

	errfunc = li_lua_push_traceback(L, 0);
	if (lua_pcall(L, 0, 0, errfunc)) {
		ERROR(srv, "lua_pcall(): %s", lua_tostring(L, -1));
		goto failed_unlock_lua;
	}
	lua_remove(L, errfunc);

	if (NULL == (lp = lua_plugin_create_data(srv, L))) goto failed_unlock_lua;

	if (NULL == (newp = li_plugin_register(srv, filename->str, lua_plugin_init, lp))) {
		lua_plugin_free_data(srv, lp);
		goto failed_unlock_lua;
	}

	g_ptr_array_add(mc->lua_plugins, newp);

	li_lua_restore_globals(L);
	li_lua_unlock(srv);

	lp->filename = filename;

	return TRUE;

failed_unlock_lua:
	lua_pop(L, lua_gettop(L) - lua_stack_top);
	li_lua_restore_globals(L);
	li_lua_unlock(srv);

	g_string_free(filename, TRUE);

	return FALSE;
}

static gboolean lua_plugin(liServer *srv, liPlugin *p, liValue *val, gpointer userdata) {
	liValue *v_filename = NULL, *v_options = NULL;
	UNUSED(userdata);

	if (val) {
		if (val->type == LI_VALUE_STRING) {
			v_filename = val;
		} else if (val->type == LI_VALUE_LIST) {
			GArray *l = val->data.list;
			if (l->len > 0) v_filename = g_array_index(l, liValue*, 0);
			if (l->len > 1) v_options = g_array_index(l, liValue*, 1);
		}
	}

	if (v_filename && v_filename->type != LI_VALUE_STRING) {
		v_filename = NULL;
	}

	if (!v_filename) {
		ERROR(srv, "%s", "lua.plugin expects at least a filename, or a filename and some options");
		return FALSE;
	}
	if (v_options && v_options->type != LI_VALUE_HASH) {
		ERROR(srv, "%s", "lua.plugin expects options in a hash");
		return FALSE;
	}

	if (v_options) {
		GHashTable *ht = v_options->data.hash;
		GHashTableIter it;
		gpointer pkey, pvalue;

		g_hash_table_iter_init(&it, ht);
		while (g_hash_table_iter_next(&it, &pkey, &pvalue)) {
			GString *key = pkey;
/*
			liValue *value = pvalue;
*/

/*
			if (g_string_equal(key, &lon_ttl)) {
				if (value->type != LI_VALUE_NUMBER || value->data.number <= 0) {
					ERROR(srv, "lua.plugin option '%s' expects positive integer as parameter", lon_ttl.str);
					return FALSE;
				}
				ttl = value->data.number;
			} else {
*/
				ERROR(srv, "unknown option for lua.plugin '%s'", key->str);
				return FALSE;
/*
			}
*/
		}
	}

	return lua_plugin_load(srv, p, li_value_extract_ptr(v_filename));
}

static const liPluginOption options[] = {
	{ NULL, 0, NULL, NULL, NULL }
};

static const liPluginAction actions[] = {
	{ "lua.handler", lua_handler_create, NULL },

	{ NULL, NULL, NULL }
};

static const liPluginSetup setups[] = {
	{ "lua.plugin", lua_plugin, NULL },

	{ NULL, NULL, NULL }
};


static void plugin_lua_init(liServer *srv, liPlugin *p, gpointer userdata) {
	UNUSED(srv); UNUSED(userdata);

	p->options = options;
	p->actions = actions;
	p->setups = setups;
}


gboolean mod_lua_init(liModules *mods, liModule *mod) {
	liPlugin *p;
	UNUSED(mod);

	MODULE_VERSION_CHECK(mods);

	p = li_plugin_register(mods->main, "mod_lua", plugin_lua_init, NULL);

	if (NULL != p) {
		module_config *mc = g_slice_new0(module_config);
		mc->lua_plugins = g_ptr_array_new();
		mc->main_plugin = p;

		p->data = mc;
		mod->config = mc;
	}

	return mod->config != NULL;
}

gboolean mod_lua_free(liModules *mods, liModule *mod) {
	if (mod->config) {
		module_config *mc = mod->config;
		guint i;

		li_plugin_free(mods->main, mc->main_plugin);
		for (i = 0; i < mc->lua_plugins->len; i++) {
			li_plugin_free(mods->main, g_ptr_array_index(mc->lua_plugins, i));
		}
		g_ptr_array_free(mc->lua_plugins, TRUE);

		g_slice_free(module_config, mc);
	}

	return TRUE;
}

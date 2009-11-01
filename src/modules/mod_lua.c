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
 *         - Basically the same as include_lua, but loads the script in a worker
 *           specific lua_State, so it doesn't use the server wide lua lock.
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

LI_API gboolean mod_lua_init(liModules *mods, liModule *mod);
LI_API gboolean mod_lua_free(liModules *mods, liModule *mod);

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

static liAction* lua_handler_create(liServer *srv, liPlugin* p, liValue *val) {
	liValue *v_filename = NULL, *v_options = NULL;
	lua_config *conf;
	guint ttl = 0;
	UNUSED(srv); UNUSED(p);

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


static const liPluginOption options[] = {
	{ NULL, 0, NULL, NULL, NULL }
};

static const liPluginAction actions[] = {
	{ "lua.handler", lua_handler_create },

	{ NULL, NULL }
};

static const liPluginSetup setups[] = {
	{ NULL, NULL }
};


static void plugin_lua_init(liServer *srv, liPlugin *p) {
	UNUSED(srv);

	p->options = options;
	p->actions = actions;
	p->setups = setups;
}


gboolean mod_lua_init(liModules *mods, liModule *mod) {
	UNUSED(mod);

	MODULE_VERSION_CHECK(mods);

	mod->config = li_plugin_register(mods->main, "mod_lua", plugin_lua_init);

	return mod->config != NULL;
}

gboolean mod_lua_free(liModules *mods, liModule *mod) {
	if (mod->config)
		li_plugin_free(mods->main, mod->config);

	return TRUE;
}

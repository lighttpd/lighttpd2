/*
 * mod_vhost - virtual hosting
 *
 * Description:
 *     mod_vhost offers various ways to implement virtual webhosts.
 *     It can map hostnames to actions and offers multiple ways to do so.
 *     These ways differ in the flexibility of mapping (what to map and what to map to) as well as performance.
 *
 * Setups:
 *     none
 * Options:
 *     vhost.debug = <true|false> - enable debug output
 * Actions:
 *     vhost.map ( "host1" => action1, "host2" => action2, "default" => action0 );
 *         - lookup action by using the hostname as the key of the hashtable
 *         - if not found, use default action
 *         - fast and flexible but no matching on hostnames possible
 *     vhost.map_regex ( "host1regex" => action1, "host2regex" => action2, "default" => action0 );
 *         - lookup action by traversing the list and applying a regex match of the hostname on each entry
 *         - uses first matching entry; if no match, use default action
 *         - slowest method but the most flexible one
 *
 * Example config:
 *
 *     mydom1 {...} mydom2 {...} defaultdom {...}
 *     vhost.map ( "dom1.com" => mydom1, "dom2.tld" => mydom2, "default" => defaultdom );
 *     vhost.map_regex ( "^(.+\.)?dom1\.com$" => mydom1, "^dom2\.(com|net|org)$" => mydom2, "default" => defaultdom );
 *
 * Tip:
 *     You can combine vhost.map and vhost.map_regex to create a reasonably fast and flexible vhost mapping mechanism.
 *     Just use a vhost.map_regex action as the default fallback action in vhost.map.
 *     This way, the expensive vhost.map_regex is only used if the vhost was not found in vhost.map.
 *
 * Todo:
 *     -
 *
 * Author:
 *     Copyright (c) 2009 Thomas Porzelt
 * License:
 *     MIT, see COPYING file in the lighttpd 2 tree
 */

#include <lighttpd/base.h>

LI_API gboolean mod_vhost_init(liModules *mods, liModule *mod);
LI_API gboolean mod_vhost_free(liModules *mods, liModule *mod);

typedef struct vhost_map_data vhost_map_data;
struct vhost_map_data {
	liPlugin *plugin;
	GHashTable *hash;
	liValue *default_action;
};

typedef struct vhost_map_regex_entry vhost_map_regex_entry;
struct vhost_map_regex_entry {
	GRegex *regex;
	liValue *action;
};

typedef struct vhost_map_regex_data vhost_map_regex_data;
struct vhost_map_regex_data {
	liPlugin *plugin;
	GArray *list; /* array of vhost_map_regex_entry */
	liValue *default_action;
};

static liHandlerResult vhost_map(liVRequest *vr, gpointer param, gpointer *context) {
	liValue *v;
	vhost_map_data *md = param;
	gboolean debug = _OPTION(vr, md->plugin, 0).boolean;
	UNUSED(context);

	v = g_hash_table_lookup(md->hash, vr->request.uri.host);

	if (NULL != v) {
		if (debug) {
			VR_DEBUG(vr, "vhost_map: host %s found in hashtable", vr->request.uri.host->str);
		}
		li_action_enter(vr, v->data.val_action.action);
	} else if (NULL != md->default_action) {
		if (debug) {
			VR_DEBUG(vr, "vhost_map: host %s not found in hashtable, executing default action", vr->request.uri.host->str);
		}
		li_action_enter(vr, md->default_action->data.val_action.action);
	} else {
		if (debug) {
			VR_DEBUG(vr, "vhost_map: neither host %s found in hashtable nor default action specified, doing nothing", vr->request.uri.host->str);
		}
	}

	return LI_HANDLER_GO_ON;
}

static void vhost_map_free(liServer *srv, gpointer param) {
	vhost_map_data *md = param;

	UNUSED(srv);

	g_hash_table_destroy(md->hash);

	if (NULL != md->default_action)
		li_value_free(md->default_action);

	g_slice_free(vhost_map_data, md);
}

static liAction* vhost_map_create(liServer *srv, liWorker *wrk, liPlugin* p, liValue *val, gpointer userdata) {
	vhost_map_data *md;
	UNUSED(wrk); UNUSED(userdata);

	val = li_value_get_single_argument(val);

	if (NULL == (val = li_value_to_key_value_list(val))) {
		ERROR(srv, "%s", "vhost.map expects a hashtable/key-value list as parameter");
		return NULL;
	}

	md = g_slice_new0(vhost_map_data);
	md->plugin = p;
	md->hash = li_value_new_hashtable();

	LI_VALUE_FOREACH(entry, val)
		liValue *entryKey = li_value_list_at(entry, 0);
		liValue *entryValue = li_value_list_at(entry, 1);
		GString *entryKeyStr;

		if (LI_VALUE_ACTION != li_value_type(entryValue)) {
			ERROR(srv, "vhost.map expects a hashtable/key-value list with action values as parameter, %s value given", li_value_type_string(entryValue));
			vhost_map_free(srv, md);
			return NULL;
		}

		/* we now own the key string: free it in case of failure */
		entryKeyStr = li_value_extract_string(entryKey);

		if (NULL != entryKeyStr && g_str_equal(entryKeyStr->str, "default")) {
			WARNING(srv, "%s", "vhost.map: found entry with string key \"default\". please convert the parameter to a key-value list and use the keyword default instead.");
			/* TODO: remove support for "default" (LI_VALUE_HASH) */
			g_string_free(entryKeyStr, TRUE);
			entryKeyStr = NULL;
		}
		if (NULL == entryKeyStr) {
			if (NULL != md->default_action) {
				ERROR(srv, "%s", "vhost.map: already have a default action");
				/* key string is NULL, nothing to free */
				vhost_map_free(srv, md);
				return NULL;
			}
			md->default_action = li_value_extract(entryValue);
		} else {
			if (NULL != g_hash_table_lookup(md->hash, entryKeyStr)) {
				ERROR(srv, "vhost.map: duplicate entry for '%s'", entryKeyStr->str);
				g_string_free(entryKeyStr, TRUE);
				vhost_map_free(srv, md);
				return NULL;
			}
			g_hash_table_insert(md->hash, entryKeyStr, li_value_extract(entryValue));
		}
	LI_VALUE_END_FOREACH()

	return li_action_new_function(vhost_map, NULL, vhost_map_free, md);
}

static liHandlerResult vhost_map_regex(liVRequest *vr, gpointer param, gpointer *context) {
	guint i;
	vhost_map_regex_data *mrd = param;
	GArray *list = mrd->list;
	gboolean debug = _OPTION(vr, mrd->plugin, 0).boolean;
	liValue *v = NULL;
	vhost_map_regex_entry *entry = NULL;

	UNUSED(context);

	/* loop through all rules to find a match */
	for (i = 0; i < list->len; i++) {
		entry = &g_array_index(list, vhost_map_regex_entry, i);

		if (!g_regex_match(entry->regex, vr->request.uri.host->str, 0, NULL))
			continue;

		v = entry->action;

		break;
	}

	if (NULL != v) {
		if (debug) {
			VR_DEBUG(vr, "vhost_map_regex: host %s matches pattern \"%s\"", vr->request.uri.host->str, g_regex_get_pattern(entry->regex));
		}
		li_action_enter(vr, v->data.val_action.action);
	} else if (NULL != mrd->default_action) {
		if (debug) {
			VR_DEBUG(vr, "vhost_map_regex: host %s didn't match, executing default action", vr->request.uri.host->str);
		}
		li_action_enter(vr, mrd->default_action->data.val_action.action);
	} else {
		if (debug) {
			VR_DEBUG(vr, "vhost_map_regex: neither did %s match nor default action specified, doing nothing", vr->request.uri.host->str);
		}
	}

	return LI_HANDLER_GO_ON;
}

static void vhost_map_regex_free(liServer *srv, gpointer param) {
	guint i;
	vhost_map_regex_data *mrd = param;
	GArray *list = mrd->list;
	UNUSED(srv);

	for (i = 0; i < list->len; i++) {
		vhost_map_regex_entry *entry = &g_array_index(list, vhost_map_regex_entry, i);

		g_regex_unref(entry->regex);
		li_value_free(entry->action);
	}
	g_array_free(list, TRUE);

	if (NULL != mrd->default_action) {
		li_value_free(mrd->default_action);
	}

	g_slice_free(vhost_map_regex_data, mrd);
}

static liAction* vhost_map_regex_create(liServer *srv, liWorker *wrk, liPlugin* p, liValue *val, gpointer userdata) {
	vhost_map_regex_data *mrd;
	UNUSED(wrk); UNUSED(userdata);

	val = li_value_get_single_argument(val);

	if (NULL == (val = li_value_to_key_value_list(val))) {
		ERROR(srv, "%s", "vhost.map_regex expects a hashtable/key-value list as parameter");
		return NULL;
	}

	mrd = g_slice_new0(vhost_map_regex_data);
	mrd->plugin = p;
	mrd->list = g_array_new(FALSE, FALSE, sizeof(vhost_map_regex_entry));

	LI_VALUE_FOREACH(entry, val)
		liValue *entryKey = li_value_list_at(entry, 0);
		liValue *entryValue = li_value_list_at(entry, 1);
		GString *entryKeyStr;

		if (LI_VALUE_ACTION != li_value_type(entryValue)) {
			ERROR(srv, "vhost.map_regex expects a hashtable/key-value list with action values as parameter, %s value given", li_value_type_string(entryValue));
			vhost_map_free(srv, mrd);
			return NULL;
		}

		/* we now own the key string: free it in case of failure */
		entryKeyStr = li_value_extract_string(entryKey);

		if (NULL != entryKeyStr && g_str_equal(entryKeyStr->str, "default")) {
			WARNING(srv, "%s", "vhost.map_regex: found entry with string key \"default\". please convert the parameter to a key-value list and use the keyword default instead.");
			/* TODO: remove support for "default" (LI_VALUE_HASH) */
			g_string_free(entryKeyStr, TRUE);
			entryKeyStr = NULL;
		}
		if (NULL == entryKeyStr) {
			if (NULL != mrd->default_action) {
				ERROR(srv, "%s", "vhost.map_regex: already have a default action");
				vhost_map_free(srv, mrd);
				return NULL;
			}
			mrd->default_action = li_value_extract(entryValue);
		} else {
			GError *err = NULL;
			vhost_map_regex_entry map_entry;

			map_entry.regex = g_regex_new(entryKeyStr->str, G_REGEX_RAW | G_REGEX_OPTIMIZE, 0, &err);
			g_string_free(entryKeyStr, TRUE);

			if (NULL == map_entry.regex) {
				assert(NULL != err);
				vhost_map_regex_free(srv, mrd);
				ERROR(srv, "vhost.map_regex: error compiling regex \"%s\": %s", entryKeyStr->str, err->message);
				g_error_free(err);
				return NULL;
			}
			assert(NULL == err);

			map_entry.action = li_value_extract(entryValue);

			g_array_append_val(mrd->list, map_entry);
		}
	LI_VALUE_END_FOREACH()

	return li_action_new_function(vhost_map_regex, NULL, vhost_map_regex_free, mrd);
}


static const liPluginOption options[] = {
	{ "vhost.debug", LI_VALUE_BOOLEAN, FALSE, NULL },

	{ NULL, 0, 0, NULL }
};

static const liPluginAction actions[] = {
	{ "vhost.map", vhost_map_create, NULL },
	{ "vhost.map_regex", vhost_map_regex_create, NULL },

	{ NULL, NULL, NULL }
};

static const liPluginSetup setups[] = {
	{ NULL, NULL, NULL }
};


static void plugin_vhost_init(liServer *srv, liPlugin *p, gpointer userdata) {
	UNUSED(srv); UNUSED(userdata);

	p->options = options;
	p->actions = actions;
	p->setups = setups;
}


gboolean mod_vhost_init(liModules *mods, liModule *mod) {
	UNUSED(mod);

	MODULE_VERSION_CHECK(mods);

	mod->config = li_plugin_register(mods->main, "mod_vhost", plugin_vhost_init, NULL);

	return mod->config != NULL;
}

gboolean mod_vhost_free(liModules *mods, liModule *mod) {
	if (mod->config)
		li_plugin_free(mods->main, mod->config);

	return TRUE;
}

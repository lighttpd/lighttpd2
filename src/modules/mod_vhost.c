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
 *     vhost.map ["host1": action1, "host2": action2, "default": action0];
 *         - lookup action by using the hostname as the key of the hashtable
 *         - if not found, use default action
 *         - fast and flexible but no matching on hostnames possible
 *     vhost.map_regex ["host1regex": action1, "host2regex": action2, "default": action0];
 *         - lookup action by traversing the list and applying a regex match of the hostname on each entry
 *         - if no match, use default action
 *         - slowest method but the most flexible one
 *         - somewhat optimized internally and automatically to speed up lookup of frequently accessed hosts
 *
 * Example config:
 *
 *     mydom1 {...} mydom2 {...} defaultdom {...}
 *     vhost.map ["dom1.com": mydom1, "dom2.tld": mydom2, "default": defaultdom];
 *     vhost.map_regex ["^(.+\.)?dom1\.com$": mydom1, "^dom2\.(com|net|org)$": mydom2, "default": defaultdom];
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
	ev_tstamp tstamp;
	guint hits;
	guint hits_30s;
};

typedef struct vhost_map_regex_data vhost_map_regex_data;
struct vhost_map_regex_data {
	liPlugin *plugin;
	GArray *lists; /* array of array of vhost_map_regex_entry */
	liValue *default_action;
};

static liHandlerResult vhost_map(liVRequest *vr, gpointer param, gpointer *context) {
	liValue *v;
	vhost_map_data *md = param;
	gboolean debug = _OPTION(vr, md->plugin, 0).boolean;

	UNUSED(context);

	v = g_hash_table_lookup(md->hash, vr->request.uri.host);

	if (v) {
		if (debug)
			VR_DEBUG(vr, "vhost_map: host %s found in hashtable", vr->request.uri.host->str);
		li_action_enter(vr, v->data.val_action.action);
	} else if (md->default_action) {
		if (debug)
			VR_DEBUG(vr, "vhost_map: host %s not found in hashtable, executing default action", vr->request.uri.host->str);
		li_action_enter(vr, md->default_action->data.val_action.action);
	} else {
		if (debug)
			VR_DEBUG(vr, "vhost_map: neither host %s found in hashtable nor default action specified, doing nothing", vr->request.uri.host->str);
	}

	return LI_HANDLER_GO_ON;
}

static void vhost_map_free(liServer *srv, gpointer param) {
	vhost_map_data *md = param;

	UNUSED(srv);

	g_hash_table_destroy(md->hash);

	g_slice_free(vhost_map_data, md);
}

static liAction* vhost_map_create(liServer *srv, liWorker *wrk, liPlugin* p, liValue *val, gpointer userdata) {
	GHashTableIter iter;
	gpointer k, v;
	vhost_map_data *md;
	GString *str;
	UNUSED(wrk); UNUSED(userdata);

	if (!val || val->type != LI_VALUE_HASH) {
		ERROR(srv, "%s", "vhost.map expects a hashtable as parameter");
		return NULL;
	}

	md = g_slice_new0(vhost_map_data);
	md->plugin = p;
	md->hash = li_value_extract_hash(val);
	str = g_string_new_len(CONST_STR_LEN("default"));
	md->default_action = g_hash_table_lookup(md->hash, str);
	g_string_free(str, TRUE);

	/* check if every value in the hashtable is an action */
	g_hash_table_iter_init(&iter, md->hash);
	while (g_hash_table_iter_next(&iter, &k, &v)) {
		val = v;

		if (val->type != LI_VALUE_ACTION) {
			ERROR(srv, "vhost.map expects a hashtable with action values as parameter, %s value given", li_value_type_string(val->type));
			vhost_map_free(srv, md);
			return NULL;
		}
	}

	return li_action_new_function(vhost_map, NULL, vhost_map_free, md);
}

static liHandlerResult vhost_map_regex(liVRequest *vr, gpointer param, gpointer *context) {
	guint i;
	ev_tstamp now;
	vhost_map_regex_data *mrd = param;
	GArray *list = g_array_index(mrd->lists, GArray*, vr->wrk->ndx);
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

		/* match found, update stats */
		now = CUR_TS(vr->wrk);
		entry->hits++;

		if ((now - entry->tstamp) > 30.0) {
			vhost_map_regex_entry *entry_prev, entry_tmp;

			entry->tstamp = now;
			entry->hits_30s = entry->hits;
			entry->hits = 0;

			if (i) {
				entry_prev = &g_array_index(list, vhost_map_regex_entry, i-1);

				if ((now - entry_prev->tstamp) > 30.0) {
					entry_prev->tstamp = now;
					entry_prev->hits_30s = entry_prev->hits;
					entry_prev->hits = 0;
				}

				/* reorder list and put entries with more hits at the beginning */
				if (entry->hits_30s > entry_prev->hits_30s) {
					/* swap entry and entry_prev */
					entry_tmp = *entry_prev;
					g_array_index(list, vhost_map_regex_entry, i-1) = *entry;
					g_array_index(list, vhost_map_regex_entry, i) = entry_tmp;
					entry = &g_array_index(list, vhost_map_regex_entry, i-1);
				}
			}
		}

		break;
	}

	if (v) {
		if (debug)
			VR_DEBUG(vr, "vhost_map_regex: host %s matches pattern \"%s\"", vr->request.uri.host->str, g_regex_get_pattern(entry->regex));
		li_action_enter(vr, v->data.val_action.action);
	} else if (mrd->default_action) {
		if (debug)
			VR_DEBUG(vr, "vhost_map_regex: host %s didn't match, executing default action", vr->request.uri.host->str);
		li_action_enter(vr, mrd->default_action->data.val_action.action);
	} else {
		if (debug)
			VR_DEBUG(vr, "vhost_map_regex: neither did %s match nor default action specified, doing nothing", vr->request.uri.host->str);
	}

	return LI_HANDLER_GO_ON;
}

static void vhost_map_regex_free(liServer *srv, gpointer param) {
	guint i;
	vhost_map_regex_entry *entry;
	vhost_map_regex_data *mrd = param;

	UNUSED(srv);

	for (i = 0; i < g_array_index(mrd->lists, GArray*, 0)->len; i++) {
		entry = &g_array_index(g_array_index(mrd->lists, GArray*, 0), vhost_map_regex_entry, i);

		g_regex_unref(entry->regex);
		li_value_free(entry->action);
	}

	g_array_free(g_array_index(mrd->lists, GArray*, 0), TRUE);

	for (i = 1; i < mrd->lists->len; i++) {
		g_array_free(g_array_index(mrd->lists, GArray*, i), TRUE);
	}

	g_array_free(mrd->lists, TRUE);

	if (mrd->default_action)
		li_value_free(mrd->default_action);

	g_slice_free(vhost_map_regex_data, mrd);
}

static liAction* vhost_map_regex_create(liServer *srv, liWorker *wrk, liPlugin* p, liValue *val, gpointer userdata) {
	GHashTable *hash;
	GHashTableIter iter;
	gpointer k, v;
	vhost_map_regex_data *mrd;
	vhost_map_regex_entry entry;
	GArray *list;
	guint i;
	GError *err = NULL;
	UNUSED(wrk); UNUSED(userdata);

	if (!val || val->type != LI_VALUE_HASH) {
		ERROR(srv, "%s", "vhost.map_regex expects a hashtable as parameter");
		return NULL;
	}

	mrd = g_slice_new0(vhost_map_regex_data);
	mrd->plugin = p;
	mrd->lists = g_array_sized_new(FALSE, FALSE, sizeof(GArray*), srv->worker_count ? srv->worker_count : 1);

	list = g_array_new(FALSE, FALSE, sizeof(vhost_map_regex_entry));

	hash = val->data.hash;

	/* check if every value in the hashtable is an action */
	g_hash_table_iter_init(&iter, hash);
	while (g_hash_table_iter_next(&iter, &k, &v)) {
		val = v;

		if (val->type != LI_VALUE_ACTION) {
			ERROR(srv, "vhost.map_regex expects a hashtable with action values as parameter, %s value given", li_value_type_string(val->type));
			vhost_map_regex_free(srv, mrd);
			return NULL;
		}

		if (g_str_equal(((GString*)k)->str, "default")) {
			mrd->default_action = li_value_copy(val);
			continue;
		}

		entry.hits = 0;
		entry.hits_30s = 0;
		entry.tstamp = 0.0;
		entry.regex = g_regex_new(((GString*)k)->str, G_REGEX_RAW | G_REGEX_OPTIMIZE, 0, &err);

		if (!entry.regex || err) {
			vhost_map_regex_free(srv, mrd);
			ERROR(srv, "vhost.map_regex: error compiling regex \"%s\": %s", ((GString*)k)->str, err->message);
			g_error_free(err);
			return NULL;
		}

		entry.action = li_value_copy(val);

		g_array_append_val(list, entry);
	}

	g_array_append_val(mrd->lists, list);

	for (i = 1; i < srv->worker_count; i++) {
		GArray *arr = g_array_sized_new(FALSE, FALSE, sizeof(vhost_map_regex_entry), list->len);

		g_array_append_vals(arr, list->data, list->len);
		g_array_append_val(mrd->lists, arr);
	}

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

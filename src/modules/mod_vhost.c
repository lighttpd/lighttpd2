/*
 * mod_vhost - virtual hosting
 *
 * Description:
 *     mod_vhost offers various ways to implement virtual webhosts.
 *     It can map hostnames to document roots or even actions and offers multiple ways to do so.
 *     These ways differ in the flexibility of mapping (what to map and what to map to) as well as performance.
 *
 * Setups:
 *     none
 * Options:
 *     vhost.debug = <true|false> - enable debug output
 * Actions:
 *     vhost.simple ("server-root" => string, "docroot" => string, "default" => string);
 *         - builds the document root by concatinating server-root + hostname + docroot
 *         - if the newly build docroot does not exist, repeat with "default" hostname
 *         - not very flexible but fast (use symlinks for some limited flexibility)
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
 *     vhost.simple ("server-root" => "/var/www/vhosts/", "docroot" => "/pub", "default" => "localhost");
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
 *     - use stat cache (when implemented)
 *     - add vhost.map_regex action which maps a hostname to an action using regular expression matching on a list of strings
 *       - normal hashtable lookup not possible, traverse list and apply every regex to the hostname until a match is found
 *       - optimize list by ordering it by number of matches every n seconds
 *         => frequently matched vhosts at the beginning resulting in a better hitrate
 *         - copy list, order the copy, exchange original list and copy
 *         - delete original list (now the copy) before the next optimization run or just overwrite it during the copy step
 *         - reset counter of entries that haven't been matched during the last n hours
 *       - also build a hashtable to cache lookups?
 *     - mod_evhost equivalent?
 *     - add setup actions to control vhost.map_regex caching behaviour
 *
 * Author:
 *     Copyright (c) 2009 Thomas Porzelt
 * License:
 *     MIT, see COPYING file in the lighttpd 2 tree
 */

#include <lighttpd/base.h>

LI_API gboolean mod_vhost_init(modules *mods, module *mod);
LI_API gboolean mod_vhost_free(modules *mods, module *mod);

struct vhost_simple_data {
	plugin *plugin;
	GString *default_vhost;
	GString *docroot;
	GString *server_root;
};
typedef struct vhost_simple_data vhost_simple_data;

struct vhost_map_data {
	plugin *plugin;
	GHashTable *hash;
	value *default_action;
};
typedef struct vhost_map_data vhost_map_data;


static handler_t vhost_simple(vrequest *vr, gpointer param, gpointer *context) {
	struct stat st;
	gboolean debug;
	vhost_simple_data *sd = param;

	UNUSED(context);

	debug = _OPTION(vr, sd->plugin, 0).boolean;

	/* build document root: sd->server_root + req.host + sd->docroot */
	g_string_truncate(vr->physical.doc_root, 0);
	g_string_append_len(vr->physical.doc_root, GSTR_LEN(sd->server_root));
	g_string_append_len(vr->physical.doc_root, GSTR_LEN(vr->request.uri.host));
	g_string_append_len(vr->physical.doc_root, GSTR_LEN(sd->docroot));

	/* check if directory exists. if not, fall back to default host */
	vr->physical.have_stat = FALSE; vr->physical.have_errno = FALSE;
	if (-1 == stat(vr->physical.doc_root->str, &st)) {
		if (debug)
			VR_DEBUG(vr, "vhost.simple: docroot for vhost \"%s\" does not exist, falling back to default", vr->request.uri.host->str);
		g_string_truncate(vr->physical.doc_root, sd->server_root->len);
		g_string_append_len(vr->physical.doc_root, GSTR_LEN(sd->default_vhost));
		g_string_append_len(vr->physical.doc_root, GSTR_LEN(sd->docroot));
	}

	if (debug)
		VR_DEBUG(vr, "vhost.simple: physical docroot now \"%s\"", vr->physical.doc_root->str);

	/* build physical path: docroot + uri.path */
	g_string_truncate(vr->physical.path, 0);
	g_string_append_len(vr->physical.path, GSTR_LEN(vr->physical.doc_root));
	g_string_append_len(vr->physical.path, GSTR_LEN(vr->request.uri.path));

	return HANDLER_GO_ON;
}

static void vhost_simple_free(server *srv, gpointer param) {
	vhost_simple_data *sd = param;

	UNUSED(srv);

	if (sd->default_vhost)
		g_string_free(sd->default_vhost, TRUE);
	if (sd->docroot)
		g_string_free(sd->docroot, TRUE);
	if (sd->server_root)
		g_string_free(sd->server_root, TRUE);

	g_slice_free(vhost_simple_data, sd);
}

static action* vhost_simple_create(server *srv, plugin* p, value *val) {
	guint i;
	GArray *arr;
	value *k, *v;
	vhost_simple_data *sd;
	GString **setting;

	UNUSED(srv); UNUSED(p);

	if (!val || val->type != VALUE_LIST) {
		ERROR(srv, "%s", "vhost.simple expects a list if string tuples as parameter");
		return NULL;
	}

	sd = g_slice_new0(vhost_simple_data);
	sd->plugin = p;

	arr = val->data.list;
	for (i = 0; i < arr->len; i++) {
		val = g_array_index(arr, value*, i);
		if (val->type != VALUE_LIST || val->data.list->len != 2) {
			vhost_simple_free(srv, sd);
			ERROR(srv, "%s", "vhost.simple expects a list if string tuples as parameter");
			return NULL;
		}

		k = g_array_index(val->data.list, value*, 0);
		v = g_array_index(val->data.list, value*, 1);
		if (k->type != VALUE_STRING || v->type != VALUE_STRING) {
			vhost_simple_free(srv, sd);
			ERROR(srv, "%s", "vhost.simple expects a list if string tuples as parameter");
			return NULL;
		}

		if (g_str_equal(k->data.string->str, "server-root")) {
			setting = &sd->server_root;
		} else if (g_str_equal(k->data.string->str, "docroot")) {
			setting = &sd->docroot;
		} else if (g_str_equal(k->data.string->str, "default")) {
			setting = &sd->default_vhost;
		} else {
			vhost_simple_free(srv, sd);
			ERROR(srv, "unkown setting \"%s\" for vhost.simple", k->data.string->str);
			return NULL;
		}

		if (*setting) {
			vhost_simple_free(srv, sd);
			ERROR(srv, "%s", "parameter set twice for vhost.simple");
			return NULL;
		}

		*setting = value_extract(v).string;
	}

	if (!sd->server_root || !sd->docroot || !sd->default_vhost) {
		vhost_simple_free(srv, sd);
		ERROR(srv, "%s", "missing parameter for vhost.simple. need \"server-root\", \"docroot\" and \"default\"");
		return NULL;
	}

	/* make sure server_root has a trailing slash (or whatever this OS uses as dir seperator */
	if (sd->server_root->len == 0 || sd->server_root->str[sd->server_root->len-1] != G_DIR_SEPARATOR)
		g_string_append_c(sd->server_root, G_DIR_SEPARATOR);
	/* make sure docroot begins with a slash (or whatever this OS uses as dir seperator */
	if (sd->docroot->len == 0 || sd->docroot->str[0] != G_DIR_SEPARATOR)
		g_string_prepend_c(sd->docroot, G_DIR_SEPARATOR);

	return action_new_function(vhost_simple, NULL, vhost_simple_free, sd);
}

static handler_t vhost_map(vrequest *vr, gpointer param, gpointer *context) {
	value *v;
	vhost_map_data *md = param;
	gboolean debug = _OPTION(vr, md->plugin, 0).boolean;

	UNUSED(context);

	v = g_hash_table_lookup(md->hash, vr->request.uri.host);

	if (v) {
		if (debug)
			VR_DEBUG(vr, "vhost_map: host %s found in hashtable", vr->request.uri.host->str);
		action_enter(vr, v->data.val_action.action);
	} else if (md->default_action) {
		if (debug)
			VR_DEBUG(vr, "vhost_map: host %s not found in hashtable, executing default action", vr->request.uri.host->str);
		action_enter(vr, md->default_action->data.val_action.action);
	} else {
		if (debug)
			VR_DEBUG(vr, "vhost_map: neither host %s found in hashtable nor default action specified, doing nothing", vr->request.uri.host->str);
	}

	return HANDLER_GO_ON;
}

static void vhost_map_free(server *srv, gpointer param) {
	vhost_map_data *md = param;

	UNUSED(srv);

	g_hash_table_destroy(md->hash);

	g_slice_free(vhost_map_data, md);
}

static action* vhost_map_create(server *srv, plugin* p, value *val) {
	GHashTableIter iter;
	gpointer k, v;
	vhost_map_data *md;
	GString *str;

	if (!val || val->type != VALUE_HASH) {
		ERROR(srv, "%s", "vhost.map expects a hashtable as parameter");
		return NULL;
	}

	md = g_slice_new0(vhost_map_data);
	md->plugin = p;
	md->hash = value_extract(val).hash;
	str = g_string_new_len(CONST_STR_LEN("default"));
	md->default_action = g_hash_table_lookup(md->hash, str);
	g_string_free(str, TRUE);

	/* check if every value in the hashtable is an action */
	g_hash_table_iter_init(&iter, md->hash);
	while (g_hash_table_iter_next(&iter, &k, &v)) {
		val = v;

		if (val->type != VALUE_ACTION) {
			ERROR(srv, "vhost.map expects a hashtable with action values as parameter, %s value given", value_type_string(val->type));
			vhost_map_free(srv, md);
			return NULL;
		}

		action_acquire(val->data.val_action.action);
	}

	return action_new_function(vhost_map, NULL, vhost_map_free, md);
}


static const plugin_option options[] = {
	{ "vhost.debug", VALUE_BOOLEAN, NULL, NULL, NULL },

	{ NULL, 0, NULL, NULL, NULL }
};

static const plugin_action actions[] = {
	{ "vhost.simple", vhost_simple_create },
	{ "vhost.map", vhost_map_create },

	{ NULL, NULL }
};

static const plugin_setup setups[] = {
	{ NULL, NULL }
};


static void plugin_vhost_init(server *srv, plugin *p) {
	UNUSED(srv);

	p->options = options;
	p->actions = actions;
	p->setups = setups;
}


gboolean mod_vhost_init(modules *mods, module *mod) {
	UNUSED(mod);

	MODULE_VERSION_CHECK(mods);

	mod->config = plugin_register(mods->main, "mod_vhost", plugin_vhost_init);

	return mod->config != NULL;
}

gboolean mod_vhost_free(modules *mods, module *mod) {
	if (mod->config)
		plugin_free(mods->main, mod->config);

	return TRUE;
}

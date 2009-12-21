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
 *     vhost.pattern string;
 *         - builds document root by substituting $0..$9 with parts of the hostname
 *         - parts are defined by splitting the hostname at each dot
 *         - $0 is the whole hostname, $1 the last part aka the tld, $2 the second last and so on
 *         - ${n-} is part n and all others, concatinated by dots (0 < n <= 9)
 *         - ${n-m} is parts n to m, concatinated by dots (0 < n < m <= 9)
 *
 * Example config:
 *     vhost.simple ("server-root" => "/var/www/vhosts/", "docroot" => "/pub", "default" => "localhost");
 *         - maps test.lighttpd.net to /var/www/vhosts/test.lighttpd.net/pub/
 *           and lighttpd.net to /var/www/vhosts/lighttpd.net/pub/
 *     
 *     mydom1 {...} mydom2 {...} defaultdom {...}
 *     vhost.map ["dom1.com": mydom1, "dom2.tld": mydom2, "default": defaultdom];
 *     vhost.map_regex ["^(.+\.)?dom1\.com$": mydom1, "^dom2\.(com|net|org)$": mydom2, "default": defaultdom];
 *
 *     vhost.pattern "/var/www/vhosts/$2.$1/$0/pub/";
 *         - maps test.lighttpd.net to /var/www/vhosts/lighttpd.net/test.lighttpd.net/pub/
 *           and lighttpd.net to /var/www/vhosts/lighttpd.net/lighttpd.net/pub/
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

struct vhost_simple_data {
	liPlugin *plugin;
	GString *default_vhost;
	GString *docroot;
	GString *server_root;
};
typedef struct vhost_simple_data vhost_simple_data;

struct vhost_map_data {
	liPlugin *plugin;
	GHashTable *hash;
	liValue *default_action;
};
typedef struct vhost_map_data vhost_map_data;

struct vhost_map_regex_entry {
	GRegex *regex;
	liValue *action;
	ev_tstamp tstamp;
	guint hits;
	guint hits_30s;
};
typedef struct vhost_map_regex_entry vhost_map_regex_entry;

struct vhost_map_regex_data {
	liPlugin *plugin;
	GArray *lists; /* array of array of vhost_map_regex_entry */
	liValue *default_action;
};
typedef struct vhost_map_regex_data vhost_map_regex_data;

struct vhost_pattern_part {
	enum {
		VHOST_PATTERN_STRING,
		VHOST_PATTERN_PART,
		VHOST_PATTERN_RANGE
	} type;
	union {
		GString *str;
		guint8 idx;
		struct {
			guint8 n;
			guint8 m;
		} range;
	} data;
};
typedef struct vhost_pattern_part vhost_pattern_part;

struct vhost_pattern_hostpart {
	gchar *str;
	guint len;
};
typedef struct vhost_pattern_hostpart vhost_pattern_hostpart;

struct vhost_pattern_data {
	GArray *parts;
	guint max_idx;
	liPlugin *plugin;
};
typedef struct vhost_pattern_data vhost_pattern_data;


static liHandlerResult vhost_simple(liVRequest *vr, gpointer param, gpointer *context) {
	struct stat st;
	gint err;
	gboolean debug;
	vhost_simple_data *sd = param;

	debug = _OPTION(vr, sd->plugin, 0).boolean;

	/* we use context to check if the physical path was already built */
	if (*context == NULL) {
		/* build document root: sd->server_root + req.host + sd->docroot */
		g_string_truncate(vr->physical.doc_root, 0);
		g_string_append_len(vr->physical.doc_root, GSTR_LEN(sd->server_root));
		g_string_append_len(vr->physical.doc_root, GSTR_LEN(vr->request.uri.host));
		g_string_append_len(vr->physical.doc_root, GSTR_LEN(sd->docroot));
	}

	/* check if directory exists. if not, fall back to default host */

	switch (li_stat_cache_get(vr, vr->physical.path, &st, &err, NULL)) {
	case LI_HANDLER_GO_ON: break;
	case LI_HANDLER_WAIT_FOR_EVENT:
		*context = GINT_TO_POINTER(1);
		return LI_HANDLER_WAIT_FOR_EVENT;
	default:
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

	return LI_HANDLER_GO_ON;
}

static void vhost_simple_free(liServer *srv, gpointer param) {
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

static liAction* vhost_simple_create(liServer *srv, liPlugin* p, liValue *val, gpointer userdata) {
	guint i;
	GArray *arr;
	liValue *k, *v;
	vhost_simple_data *sd;
	GString **setting;

	UNUSED(userdata);

	if (!val || val->type != LI_VALUE_LIST) {
		ERROR(srv, "%s", "vhost.simple expects a list if string tuples as parameter");
		return NULL;
	}

	sd = g_slice_new0(vhost_simple_data);
	sd->plugin = p;

	arr = val->data.list;
	for (i = 0; i < arr->len; i++) {
		val = g_array_index(arr, liValue*, i);
		if (val->type != LI_VALUE_LIST || val->data.list->len != 2) {
			vhost_simple_free(srv, sd);
			ERROR(srv, "%s", "vhost.simple expects a list if string tuples as parameter");
			return NULL;
		}

		k = g_array_index(val->data.list, liValue*, 0);
		v = g_array_index(val->data.list, liValue*, 1);
		if (k->type != LI_VALUE_STRING || v->type != LI_VALUE_STRING) {
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

		*setting = li_value_extract(v).string;
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

	return li_action_new_function(vhost_simple, NULL, vhost_simple_free, sd);
}

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

static liAction* vhost_map_create(liServer *srv, liPlugin* p, liValue *val, gpointer userdata) {
	GHashTableIter iter;
	gpointer k, v;
	vhost_map_data *md;
	GString *str;
	UNUSED(userdata);

	if (!val || val->type != LI_VALUE_HASH) {
		ERROR(srv, "%s", "vhost.map expects a hashtable as parameter");
		return NULL;
	}

	md = g_slice_new0(vhost_map_data);
	md->plugin = p;
	md->hash = li_value_extract(val).hash;
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

static liAction* vhost_map_regex_create(liServer *srv, liPlugin* p, liValue *val, gpointer userdata) {
	GHashTable *hash;
	GHashTableIter iter;
	gpointer k, v;
	vhost_map_regex_data *mrd;
	vhost_map_regex_entry entry;
	GArray *list;
	guint i;
	GError *err = NULL;
	UNUSED(userdata);

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

static liHandlerResult vhost_pattern(liVRequest *vr, gpointer param, gpointer *context) {
	GArray *parts = g_array_sized_new(FALSE, TRUE, sizeof(vhost_pattern_hostpart), 6);
	vhost_pattern_data *pattern = param;
	gboolean debug = _OPTION(vr, pattern->plugin, 0).boolean;
	guint i, j;
	gchar *c, *c_last;
	vhost_pattern_hostpart hp;

	UNUSED(context);

	if (!vr->request.uri.host->len) {
		if (debug)
			VR_DEBUG(vr, "%s", "vhost_pattern: no host given");
		return LI_HANDLER_GO_ON;
	}

	/* parse host. we traverse the host in reverse order */
	/* foo.bar.baz. */
	c = &vr->request.uri.host->str[vr->request.uri.host->len-1];
	c_last = c+1;
	for (i = 0; i < pattern->max_idx && c >= vr->request.uri.host->str; c--) {
		if (*c == '.') {
			hp.str = c+1;
			hp.len = c_last - c - 1;
			g_array_append_val(parts, hp);
			i++;
			c_last = c;
		}
	}

	if (i < pattern->max_idx) {
		/* the last part */
		hp.str = vr->request.uri.host->str;
		hp.len = c_last - hp.str;
		g_array_append_val(parts, hp);
	}

	/* now construct the new docroot */
	g_string_truncate(vr->physical.doc_root, 0);
	for (i = 0; i < pattern->parts->len; i++) {
		vhost_pattern_part *p = &g_array_index(pattern->parts, vhost_pattern_part, i);
		switch (p->type) {
		case VHOST_PATTERN_STRING:
			g_string_append_len(vr->physical.doc_root, GSTR_LEN(p->data.str));
			break;
		case VHOST_PATTERN_PART:
			if (p->data.idx == 0) {
				/* whole hostname */
				g_string_append_len(vr->physical.doc_root, GSTR_LEN(vr->request.uri.host));
			} else if (p->data.idx <= parts->len) {
				/* specific part */
				g_string_append_len(vr->physical.doc_root, g_array_index(parts, vhost_pattern_hostpart, p->data.idx-1).str, g_array_index(parts, vhost_pattern_hostpart, p->data.idx-1).len);
			}
			break;
		case VHOST_PATTERN_RANGE:
			if (p->data.range.n > parts->len)
				continue;
			for (j = MIN(p->data.range.m, parts->len); j >= p->data.range.n; j--) {
				if (j < MIN(p->data.range.m, parts->len))
					g_string_append_c(vr->physical.doc_root, '.');
				g_string_append_len(vr->physical.doc_root, g_array_index(parts, vhost_pattern_hostpart, j-1).str, g_array_index(parts, vhost_pattern_hostpart, j-1).len);
			}
			break;
		}
	}

	/* build physical path: docroot + uri.path */
	g_string_truncate(vr->physical.path, 0);
	g_string_append_len(vr->physical.path, GSTR_LEN(vr->physical.doc_root));
	g_string_append_len(vr->physical.path, GSTR_LEN(vr->request.uri.path));

	if (debug)
		VR_DEBUG(vr, "vhost.pattern: mapped host \"%s\" to docroot \"%s\"", vr->request.uri.host->str, vr->physical.doc_root->str);

	g_array_free(parts, TRUE);

	return LI_HANDLER_GO_ON;
}

static void vhost_pattern_free(liServer *srv, gpointer param) {
	vhost_pattern_data *pd = param;
	guint i;

	UNUSED(srv);

	for (i = 0; i < pd->parts->len; i++) {
		vhost_pattern_part *p = &g_array_index(pd->parts, vhost_pattern_part, i);
		if (p->type == VHOST_PATTERN_STRING) {
			g_string_free(p->data.str, TRUE);
		}
	}

	g_array_free(pd->parts, TRUE);
	g_slice_free(vhost_pattern_data, pd);
}

static liAction* vhost_pattern_create(liServer *srv, liPlugin* p, liValue *val, gpointer userdata) {
	vhost_pattern_data *pd;
	GString *str;
	gchar *c, *c_last;
	vhost_pattern_part part;
	UNUSED(userdata);

	if (!val || val->type != LI_VALUE_STRING) {
		ERROR(srv, "%s", "vhost.map expects a hashtable as parameter");
		return NULL;
	}

	str = li_value_extract(val).string;

	pd = g_slice_new0(vhost_pattern_data);
	pd->parts = g_array_sized_new(FALSE, TRUE, sizeof(vhost_pattern_part), 6);
	pd->plugin = p;

	/* parse pattern */
	for (c_last = c = str->str; *c; c++) {
		if (*c == '$') {
			if (c - c_last > 0) {
				/* normal string */
				part.type = VHOST_PATTERN_STRING;
				part.data.str = g_string_new_len(c_last, c - c_last);
				g_array_append_val(pd->parts, part);
			}

			c++;
			c_last = c+1;

			if (*c == '$') {
				/* $$ */
				if (pd->parts->len && g_array_index(pd->parts, vhost_pattern_part, pd->parts->len - 1).type == VHOST_PATTERN_STRING) {
					g_string_append_c(g_array_index(pd->parts, vhost_pattern_part, pd->parts->len - 1).data.str, '$');
					continue;
				} else if (!pd->parts->len) {
					continue;
				} else {
					part.type = VHOST_PATTERN_STRING;
					part.data.str = g_string_new_len(CONST_STR_LEN("$"));
				}

				g_array_append_val(pd->parts, part);
			}
			else if (*c >= '0' && *c <= '9') {
				/* $n */
				part.type = VHOST_PATTERN_PART;
				part.data.idx = *c - '0';
				pd->max_idx = MAX(pd->max_idx, part.data.idx);
				g_array_append_val(pd->parts, part);
			} else if (*c == '{') {
				/* ${n-} or ${n-m} */
				c++;
				if (!(*c > '0' && *c <= '9') || *(c+1) != '-') {
					vhost_pattern_free(srv, pd);
					ERROR(srv, "vhost.pattern: malformed pattern \"%s\"", str->str);
					return NULL;
				}

				part.type = VHOST_PATTERN_RANGE;

				if (*(c+2) == '}') {
					/* ${n-} */
					part.data.range.n = *c - '0';
					part.data.range.m = 9;
					c_last += 3;
					pd->max_idx = 9;
				} else if (*(c+2) > '0' && *(c+2) <= '9' && *c < *(c+2) && *(c+3) == '}') {
					/* ${n-m} */
					part.data.range.n = *c - '0';
					part.data.range.m = *(c+2) - '0';
					c_last += 4;
					pd->max_idx = MAX(pd->max_idx, part.data.range.m);
				} else {
					vhost_pattern_free(srv, pd);
					ERROR(srv, "vhost.pattern: malformed pattern \"%s\"", str->str);
					return NULL;
				}

				g_array_append_val(pd->parts, part);
			} else {
				vhost_pattern_free(srv, pd);
				ERROR(srv, "vhost.pattern: malformed pattern \"%s\"", str->str);
				return NULL;
			}
		}
	}

	if (c - c_last > 0) {
		part.type = VHOST_PATTERN_STRING;
		part.data.str = g_string_new_len(c_last, c - c_last);
		g_array_append_val(pd->parts, part);
	}

	return li_action_new_function(vhost_pattern, NULL, vhost_pattern_free, pd);
}


static const liPluginOption options[] = {
	{ "vhost.debug", LI_VALUE_BOOLEAN, NULL, NULL, NULL },

	{ NULL, 0, NULL, NULL, NULL }
};

static const liPluginAction actions[] = {
	{ "vhost.simple", vhost_simple_create, NULL },
	{ "vhost.map", vhost_map_create, NULL },
	{ "vhost.map_regex", vhost_map_regex_create, NULL },
	{ "vhost.pattern", vhost_pattern_create, NULL },

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

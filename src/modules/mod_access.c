/*
 * mod_access - restrict access to the webserver for certain clients
 *
 * Description:
 *     mod_access lets you filter clients by IP address.
 *
 * Setups:
 *     access.load "file";
 *         - Loads access rules from a file. One rule per line in the format <action> <ip>.
 *           Example file (\n is newline): allow 127.0.0.1\ndeny 10.0.0.0/8\nallow 192.168.0.0/24
 * Options:
 *     access.redirect_url = "url";
 *         - if set, clients are redirected to this url if access is refused
 * Actions:
 *     access.deny;
 *         - Denies access by returning a 403 status code
 *     access.check ("allow" => iplist, "deny" => iplist);
 *         - "allow" and "deny" are optional. If left out, they default to "all"
 *         - iplists are lists of strings representing IP addresses with optional CIDR suffix
 *         - To represent all IPs, you can either use "x.x.x.x/0" or "all"
 *
 * Example config:
 *     access.redirect_url = "http://www.example.tld/denied.html";
 *     access.check (
 *         "allow" => ("127.0.0.0/24", "192.168.0.0/16"),
 *         "deny" => "all"
 *     );
 *     if req.path =$ ".inc" { access.deny; }
 *
 *     This config snippet will grant access only to clients from the local network (127.0.0.* or 192.168.*.*).
 *     Additionally it will deny access to any file ending with ".inc", no matter what client.
 *     Clients that are denied access, will be redirected to http://www.example.tld/denied.html
 *
 * Tip:
 *     none
 *
 * Todo:
 *     - access.redirect_url
 *     - ipv6 support
 *
 * Author:
 *     Copyright (c) 2009 Thomas Porzelt
 * License:
 *     MIT, see COPYING file in the lighttpd 2 tree
 */

#include <lighttpd/base.h>
#include <lighttpd/radix.h>

LI_API gboolean mod_access_init(modules *mods, module *mod);
LI_API gboolean mod_access_free(modules *mods, module *mod);

struct access_check_data {
	plugin *p;
	RadixTree32 *ipv4;
};
typedef struct access_check_data access_check_data;


static handler_t access_check(vrequest *vr, gpointer param, gpointer *context) {
	access_check_data *acd = param;
	sock_addr *addr = vr->con->remote_addr.addr;
	gboolean log_blocked = _OPTION(vr, acd->p, 0).boolean;
	GString *redirect_url = _OPTION(vr, acd->p, 1).string;

	UNUSED(context);
	UNUSED(redirect_url);

	if (addr->plain.sa_family == AF_INET) {
		if (radixtree32_lookup(acd->ipv4, htonl(addr->ipv4.sin_addr.s_addr))) {
			vrequest_handle_direct(vr);
			vr->response.http_status = 403;

			if (log_blocked) {
				VR_INFO(vr, "access.check: blocked %s", vr->con->remote_addr_str->str);
			}
		}
	} else if (addr->plain.sa_family == AF_INET6) {
		VR_ERROR(vr, "%s", "access.check doesn't support ipv6 clients yet");
		return HANDLER_ERROR;
	} else {
		VR_ERROR(vr, "%s", "access.check only supports ipv4 or ipv6 clients");
		return HANDLER_ERROR;
	}

	return HANDLER_GO_ON;
}

static void access_check_free(server *srv, gpointer param) {
	access_check_data *acd = param;

	UNUSED(srv);

	radixtree32_free(acd->ipv4);
	g_slice_free(access_check_data, acd);
}

static action* access_check_create(server *srv, plugin* p, value *val) {
	GArray *arr;
	value *v, *ip;
	guint i, j;
	guint32 ipv4, netmaskv4;
	gboolean deny = FALSE;
	gboolean got_deny = FALSE;
	access_check_data *acd = NULL;

	UNUSED(srv);

	if (!val || val->type != VALUE_LIST || (val->data.list->len != 1 && val->data.list->len != 2)) {
		ERROR(srv, "%s", "access_check expects a list of one or two string,list tuples as parameter");
		return NULL;
	}

	arr = val->data.list;

	acd = g_slice_new0(access_check_data);
	acd->p = p;
	acd->ipv4 = radixtree32_new(2);

	for (i = 0; i < arr->len; i++) {
		v = g_array_index(arr, value*, i);

		if (v->type != VALUE_LIST || v->data.list->len != 2) {
			ERROR(srv, "%s", "access_check expects a list of one or two string,list tuples as parameter");
			radixtree32_free(acd->ipv4);
			g_slice_free(access_check_data, acd);
			return NULL;
		}

		v = g_array_index(v->data.list, value*, 0);

		if (v->type != VALUE_STRING) {
			ERROR(srv, "%s", "access_check expects a list of one or two string,list tuples as parameter");
			radixtree32_free(acd->ipv4);
			g_slice_free(access_check_data, acd);
			return NULL;
		}

		if (g_str_equal(v->data.string->str, "allow")) {
			deny = FALSE;
		} else if (g_str_equal(v->data.string->str, "deny")) {
			deny = TRUE;
			got_deny = TRUE;
		} else {
			ERROR(srv, "access_check: invalid option \"%s\"", v->data.string->str);
			radixtree32_free(acd->ipv4);
			g_slice_free(access_check_data, acd);
			return NULL;
		}

		v = g_array_index(g_array_index(arr, value*, i)->data.list, value*, 1);

		if (v->type != VALUE_LIST) {
			ERROR(srv, "%s", "access_check expects a list of one or two string,list tuples as parameter");
			radixtree32_free(acd->ipv4);
			g_slice_free(access_check_data, acd);
			return NULL;
		}

		for (j = 0; j < v->data.list->len; j++) {
			ip = g_array_index(v->data.list, value*, j);

			if (ip->type != VALUE_STRING) {
				ERROR(srv, "%s", "access_check expects a list of one or two string,list tuples as parameter");
				radixtree32_free(acd->ipv4);
				g_slice_free(access_check_data, acd);
				return NULL;
			}

			if (g_str_equal(ip->data.string->str, "all")) {
				radixtree32_insert(acd->ipv4, 0, 0x00000000, GINT_TO_POINTER(deny));
			} else if (parse_ipv4(ip->data.string->str, &ipv4, &netmaskv4, NULL)) {
				radixtree32_insert(acd->ipv4, htonl(ipv4), htonl(netmaskv4), GINT_TO_POINTER(deny));
			/*} else if (parse_ipv6(v->data.string->str, ..., NULL) {
				radixtree128_insert(acd->ipv6, ipv6, netmaskv6, (gpointer)allow;*/
			} else {
				ERROR(srv, "access_check: error parsing ip: %s", ip->data.string->str);
				radixtree32_free(acd->ipv4);
				g_slice_free(access_check_data, acd);
				return NULL;
			}
		}
	}

	if (!got_deny)
		radixtree32_insert(acd->ipv4, 0, 0x00000000, GINT_TO_POINTER(TRUE));

	return action_new_function(access_check, NULL, access_check_free, acd);
}


static handler_t access_deny(vrequest *vr, gpointer param, gpointer *context) {
	gboolean log_blocked = _OPTION(vr, ((plugin*)param), 0).boolean;
	GString *redirect_url = _OPTION(vr, ((plugin*)param), 1).string;

	UNUSED(context);
	UNUSED(redirect_url);

	vrequest_handle_direct(vr);
	vr->response.http_status = 403;

	if (log_blocked) {
		VR_INFO(vr, "access.deny: blocked %s", vr->con->remote_addr_str->str);
	}

	return HANDLER_GO_ON;
}

static action* access_deny_create(server *srv, plugin* p, value *val) {

	UNUSED(srv);

	if (val) {
		ERROR(srv, "%s", "access.deny doesn't expect any parameters");
		return NULL;
	}

	return action_new_function(access_deny, NULL, NULL, p);
}


static const plugin_option options[] = {
	{ "access.log_blocked", VALUE_BOOLEAN, NULL, NULL, NULL },
	{ "access.redirect_url", VALUE_STRING, NULL, NULL, NULL },

	{ NULL, 0, NULL, NULL, NULL }
};

static const plugin_action actions[] = {
	{ "access.check", access_check_create },
	{ "access.deny", access_deny_create },

	{ NULL, NULL }
};

static const plugin_setup setups[] = {
	{ NULL, NULL }
};


static void plugin_access_init(server *srv, plugin *p) {
	UNUSED(srv);

	p->options = options;
	p->actions = actions;
	p->setups = setups;
}


gboolean mod_access_init(modules *mods, module *mod) {
	UNUSED(mod);

	MODULE_VERSION_CHECK(mods);

	mod->config = plugin_register(mods->main, "mod_access", plugin_access_init);

	return mod->config != NULL;
}

gboolean mod_access_free(modules *mods, module *mod) {
	if (mod->config)
		plugin_free(mods->main, mod->config);

	return TRUE;
}

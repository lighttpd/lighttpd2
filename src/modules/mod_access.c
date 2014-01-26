/*
 * mod_access - restrict access to the webserver for certain clients
 *
 * Todo:
 *     - access.redirect_url
 *
 * Author:
 *     Copyright (c) 2009 Thomas Porzelt
 * License:
 *     MIT, see COPYING file in the lighttpd 2 tree
 */

#include <lighttpd/base.h>
#include <lighttpd/radix.h>

LI_API gboolean mod_access_init(liModules *mods, liModule *mod);
LI_API gboolean mod_access_free(liModules *mods, liModule *mod);

struct access_check_data {
	liPlugin *p;
	liRadixTree *ipv4, *ipv6;
};
typedef struct access_check_data access_check_data;

enum { ACCESS_DENY = 1, ACCESS_ALLOW = 2 };

enum {
	OPTION_LOG_BLOCKED = 0
};

enum {
	OPTION_REDIRECT_URL = 0
};

static liHandlerResult access_check(liVRequest *vr, gpointer param, gpointer *context) {
	access_check_data *acd = param;
	liSockAddr *addr = vr->coninfo->remote_addr.addr;
	gboolean log_blocked = _OPTION(vr, acd->p, OPTION_LOG_BLOCKED).boolean;
	GString *redirect_url = _OPTIONPTR(vr, acd->p, OPTION_REDIRECT_URL).string;

	UNUSED(context);
	UNUSED(redirect_url);

	if (addr->plain.sa_family == AF_INET) {
		if (GINT_TO_POINTER(ACCESS_DENY) == li_radixtree_lookup(acd->ipv4, &addr->ipv4.sin_addr.s_addr, 32)) {
			if (!li_vrequest_handle_direct(vr))
				return LI_HANDLER_GO_ON;

			vr->response.http_status = 403;

			if (log_blocked)
				VR_INFO(vr, "access.check: blocked %s", vr->coninfo->remote_addr_str->str);
		}
#ifdef HAVE_IPV6
	} else if (addr->plain.sa_family == AF_INET6) {
		if (GINT_TO_POINTER(ACCESS_DENY) == li_radixtree_lookup(acd->ipv6, &addr->ipv6.sin6_addr.s6_addr, 128)) {
			if (!li_vrequest_handle_direct(vr))
				return LI_HANDLER_GO_ON;

			vr->response.http_status = 403;

			if (log_blocked)
				VR_INFO(vr, "access.check: blocked %s", vr->coninfo->remote_addr_str->str);
		}
#endif
	} else {
		VR_ERROR(vr, "%s", "access.check only supports ipv4 or ipv6 clients");
		return LI_HANDLER_ERROR;
	}

	return LI_HANDLER_GO_ON;
}

static void access_check_free(liServer *srv, gpointer param) {
	access_check_data *acd = param;

	UNUSED(srv);

	li_radixtree_free(acd->ipv4, NULL, NULL);
	li_radixtree_free(acd->ipv6, NULL, NULL);
	g_slice_free(access_check_data, acd);
}

static liAction* access_check_create(liServer *srv, liWorker *wrk, liPlugin* p, liValue *val, gpointer userdata) {
	access_check_data *acd = NULL;

	UNUSED(srv); UNUSED(wrk); UNUSED(userdata);

	val = li_value_get_single_argument(val);
	if (LI_VALUE_STRING == li_value_list_type_at(val, 0)) {
		li_value_wrap_in_list(val);
	}

	if (!li_value_list_has_len(val, 1) && !li_value_list_has_len(val, 2)) {
		ERROR(srv, "%s", "access_check expects a list of one or two string,list tuples as parameter");
		return NULL;
	}

	acd = g_slice_new0(access_check_data);
	acd->p = p;
	acd->ipv4 = li_radixtree_new();
	acd->ipv6 = li_radixtree_new();
	li_radixtree_insert(acd->ipv4, NULL, 0, GINT_TO_POINTER(ACCESS_DENY));
	li_radixtree_insert(acd->ipv6, NULL, 0, GINT_TO_POINTER(ACCESS_DENY));

	LI_VALUE_FOREACH(v, val)
		liValue *vAD, *vIPs;
		gboolean deny = FALSE;

		if (!li_value_list_has_len(v, 2)) {
			ERROR(srv, "%s", "access_check expects a list of one or two string,list tuples as parameter");
			goto failed_free_acd;
		}

		vAD = li_value_list_at(v, 0);

		if (LI_VALUE_STRING != li_value_type(vAD)) {
			ERROR(srv, "%s", "access_check expects a list of one or two string,list tuples as parameter");
			goto failed_free_acd;
		}

		if (g_str_equal(vAD->data.string->str, "allow")) {
			deny = FALSE;
		} else if (g_str_equal(vAD->data.string->str, "deny")) {
			deny = TRUE;
		} else {
			ERROR(srv, "access_check: invalid option \"%s\"", vAD->data.string->str);
			goto failed_free_acd;
		}

		vIPs = li_value_list_at(v, 1);

		if (LI_VALUE_LIST != li_value_type(vIPs)) {
			ERROR(srv, "%s", "access_check expects a list of one or two string,list tuples as parameter");
			goto failed_free_acd;
		}

		LI_VALUE_FOREACH(ip, vIPs)
			guint32 ipv4, netmaskv4;
			guint8 ipv6_addr[16];
			guint ipv6_network;

			if (LI_VALUE_STRING != li_value_type(ip)) {
				ERROR(srv, "%s", "access_check expects a list of one or two string,list tuples as parameter");
				goto failed_free_acd;
			}

			if (g_str_equal(ip->data.string->str, "all")) {
				li_radixtree_insert(acd->ipv4, NULL, 0, GINT_TO_POINTER(deny ? ACCESS_DENY : ACCESS_ALLOW));
				li_radixtree_insert(acd->ipv6, NULL, 0, GINT_TO_POINTER(deny ? ACCESS_DENY : ACCESS_ALLOW));
			} else if (li_parse_ipv4(ip->data.string->str, &ipv4, &netmaskv4, NULL)) {
				gint prefixlen;
				netmaskv4 = ntohl(netmaskv4);
				prefixlen = 32 - g_bit_nth_lsf(netmaskv4, -1);
				if (prefixlen < 0 || prefixlen > 32) prefixlen = 0;
				li_radixtree_insert(acd->ipv4, &ipv4, prefixlen, GINT_TO_POINTER(deny ? ACCESS_DENY : ACCESS_ALLOW));
			} else if (li_parse_ipv6(ip->data.string->str, ipv6_addr, &ipv6_network, NULL)) {
				li_radixtree_insert(acd->ipv6, ipv6_addr, ipv6_network, GINT_TO_POINTER(deny ? ACCESS_DENY : ACCESS_ALLOW));
			} else {
				ERROR(srv, "access_check: error parsing ip: %s", ip->data.string->str);
				goto failed_free_acd;
			}
		LI_VALUE_END_FOREACH()
	LI_VALUE_END_FOREACH()

	return li_action_new_function(access_check, NULL, access_check_free, acd);

failed_free_acd:
	li_radixtree_free(acd->ipv4, NULL, NULL);
	li_radixtree_free(acd->ipv6, NULL, NULL);
	g_slice_free(access_check_data, acd);
	return NULL;
}


static liHandlerResult access_deny(liVRequest *vr, gpointer param, gpointer *context) {
	gboolean log_blocked = _OPTION(vr, ((liPlugin*)param), OPTION_LOG_BLOCKED).boolean;
	GString *redirect_url = _OPTIONPTR(vr, ((liPlugin*)param), OPTION_REDIRECT_URL).string;

	UNUSED(context);
	UNUSED(redirect_url);

	if (!li_vrequest_handle_direct(vr))
		return LI_HANDLER_GO_ON;

	vr->response.http_status = 403;

	if (log_blocked) {
		VR_INFO(vr, "access.deny: blocked %s", vr->coninfo->remote_addr_str->str);
	}

	return LI_HANDLER_GO_ON;
}

static liAction* access_deny_create(liServer *srv, liWorker *wrk, liPlugin* p, liValue *val, gpointer userdata) {
	UNUSED(srv); UNUSED(wrk); UNUSED(userdata);

	if (!li_value_is_nothing(val)) {
		ERROR(srv, "%s", "access.deny doesn't expect any parameters");
		return NULL;
	}

	return li_action_new_function(access_deny, NULL, NULL, p);
}


static const liPluginOption options[] = {
	{ "access.log_blocked", LI_VALUE_BOOLEAN, 0, NULL },

	{ NULL, 0, 0, NULL }
};

static const liPluginOptionPtr optionptrs[] = {
	{ "access.redirect_url", LI_VALUE_STRING, NULL, NULL, NULL },

	{ NULL, 0, NULL, NULL, NULL }
};

static const liPluginAction actions[] = {
	{ "access.check", access_check_create, NULL },
	{ "access.deny", access_deny_create, NULL },

	{ NULL, NULL, NULL }
};

static const liPluginSetup setups[] = {
	{ NULL, NULL, NULL }
};


static void plugin_access_init(liServer *srv, liPlugin *p, gpointer userdata) {
	UNUSED(srv); UNUSED(userdata);

	p->options = options;
	p->optionptrs = optionptrs;
	p->actions = actions;
	p->setups = setups;
}


gboolean mod_access_init(liModules *mods, liModule *mod) {
	UNUSED(mod);

	MODULE_VERSION_CHECK(mods);

	mod->config = li_plugin_register(mods->main, "mod_access", plugin_access_init, NULL);

	return mod->config != NULL;
}

gboolean mod_access_free(liModules *mods, liModule *mod) {
	if (mod->config)
		li_plugin_free(mods->main, mod->config);

	return TRUE;
}

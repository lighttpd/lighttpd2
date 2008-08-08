
#include "base.h"
#include "plugin_core.h"

static action* core_list(server *srv, plugin* p, option *opt) {
	action *a;
	guint i;
	UNUSED(p);

	if (opt->type == OPTION_ACTION) {
		a = opt->value.opt_action.action;
		action_acquire(a);
		return a;
	}

	if (opt->type != OPTION_LIST) {
		ERROR(srv, "expected list, got %s", option_type_string(opt->type));
		return NULL;
	}

	a = action_new_list();
	for (i = 0; i < opt->value.opt_list->len; i++) {
		option *oa = g_array_index(opt->value.opt_list, option*, i);
		if (oa->type != OPTION_ACTION) {
			ERROR(srv, "expected action at entry %u of list, got %s", i, option_type_string(oa->type));
			action_release(srv, a);
			return NULL;
		}
		assert(srv == oa->value.opt_action.srv);
		action_acquire(oa->value.opt_action.action);
		g_array_append_val(a->value.list, oa->value.opt_action.action);
	}
	option_free(opt);
	return a;
}

static action* core_when(server *srv, plugin* p, option *opt) {
	option *opt_cond, *opt_act;
	action *a;
	UNUSED(p);

	if (opt->type != OPTION_LIST) {
		ERROR(srv, "expected list, got %s", option_type_string(opt->type));
		return NULL;
	}
	if (opt->value.opt_list->len != 2) {
		ERROR(srv, "expected list with length 2, has length %u", opt->value.opt_list->len);
		return NULL;
	}
	opt_cond = g_array_index(opt->value.opt_list, option*, 0);
	opt_act = g_array_index(opt->value.opt_list, option*, 1);
	if (opt_cond->type != OPTION_CONDITION) {
		ERROR(srv, "expected condition as first parameter, got %s", option_type_string(opt_cond->type));
		return NULL;
	}
	if (opt_act->type != OPTION_ACTION) {
		ERROR(srv, "expected action as second parameter, got %s", option_type_string(opt_act->type));
		return NULL;
	}
	condition_acquire(opt_cond->value.opt_cond.cond);
	action_acquire(opt_act->value.opt_action.action);
	a = action_new_condition(opt_cond->value.opt_cond.cond, opt_act->value.opt_action.action);
	option_free(opt);
	return a;
}

static action* core_set(server *srv, plugin* p, option *opt) {
	option *value, *opt_name;
	action *a;
	UNUSED(p);

	if (opt->type != OPTION_LIST) {
		ERROR(srv, "expected list, got %s", option_type_string(opt->type));
		return NULL;
	}
	if (opt->value.opt_list->len != 2) {
		ERROR(srv, "expected list with length 2, has length %u", opt->value.opt_list->len);
		return NULL;
	}
	opt_name = g_array_index(opt->value.opt_list, option*, 0);
	value = g_array_index(opt->value.opt_list, option*, 1);
	if (opt_name->type != OPTION_STRING) {
		ERROR(srv, "expected string as first parameter, got %s", option_type_string(opt_name->type));
		return NULL;
	}
	a = option_action(srv, opt_name->value.opt_string->str, value);
	option_free(opt);
	return a;
}

static action_result core_handle_static(server *srv, connection *con, gpointer param) {
	UNUSED(param);
	/* TODO: handle static files */
	CON_ERROR(srv, con, "%s", "Not implemented yet");
	return ACTION_ERROR;
}

static action* core_static(server *srv, plugin* p, option *opt) {
	UNUSED(p);
	if (opt) {
		ERROR(srv, "%s", "static action doesn't have parameters");
		return NULL;
	}

	return action_new_function(core_handle_static, NULL, NULL);
}

static action_result core_handle_test(server *srv, connection *con, gpointer param) {
	UNUSED(param);

	if (con->state != CON_STATE_HANDLE_REQUEST_HEADER) return ACTION_GO_ON;

	con->response.http_status = 200;
	chunkqueue_append_mem(con->out, GSTR_LEN(con->request.uri.uri));
	chunkqueue_append_mem(con->out, CONST_STR_LEN("\r\n"));
	connection_handle_direct(srv, con);

	return ACTION_GO_ON;
}

static action* core_test(server *srv, plugin* p, option *opt) {
	UNUSED(p);

	if (opt) {
		ERROR(srv, "%s", "static action doesn't have parameters");
		return NULL;
	}

	return action_new_function(core_handle_test, NULL, NULL);
}

static gboolean core_listen(server *srv, plugin* p, option *opt) {
	guint32 ipv4;
	guint8 ipv6[16];
	UNUSED(p);
	if (opt->type != OPTION_STRING) {
		ERROR(srv, "%s", "listen expects a string as parameter");
		return FALSE;
	}

	if (parse_ipv4(opt->value.opt_string->str, &ipv4, NULL)) {
		int s, val;
		struct sockaddr_in addr;
		memset(&addr, 0, sizeof(addr));
		addr.sin_family = AF_INET;
		addr.sin_addr.s_addr = ipv4;
		addr.sin_port = htons(8080);
		if (-1 == (s = socket(AF_INET, SOCK_STREAM, 0))) {
			ERROR(srv, "Couldn't open socket: %s", g_strerror(errno));
			return FALSE;
		}
		val = 1;
		if (-1 == setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val))) {
			close(s);
			ERROR(srv, "Couldn't setsockopt(SO_REUSEADDR) to '%s': %s", inet_ntoa(*(struct in_addr*)&ipv4), g_strerror(errno));
			return FALSE;
		}
		if (-1 == bind(s, &addr, sizeof(addr))) {
			close(s);
			ERROR(srv, "Couldn't bind socket to '%s': %s", inet_ntoa(*(struct in_addr*)&ipv4), g_strerror(errno));
			return FALSE;
		}
		if (-1 == listen(s, 1000)) {
			close(s);
			ERROR(srv, "Couldn't listen on '%s': %s", inet_ntoa(*(struct in_addr*)&ipv4), g_strerror(errno));
			return FALSE;
		}
		server_listen(srv, s);
		TRACE(srv, "listen to ipv4: '%s'", inet_ntoa(*(struct in_addr*)&ipv4));
#ifdef HAVE_IPV6
	} else if (parse_ipv6(opt->value.opt_string->str, ipv6, NULL)) {
		/* TODO: IPv6 */
		ERROR(srv, "%s", "IPv6 not supported yet");
		return FALSE;
#endif
	} else {
		ERROR(srv, "Invalid ip: '%s'", opt->value.opt_string->str);
		return FALSE;
	}

	TRACE(srv, "will listen to '%s'", opt->value.opt_string->str);
	option_free(opt);
	return TRUE;
}

static const plugin_option options[] = {
	{ "debug.log-request-handling", OPTION_BOOLEAN, NULL, NULL},
	{ "log.level", OPTION_STRING, NULL, NULL },

	{ "static-file.exclude", OPTION_LIST, NULL, NULL },
	{ NULL, 0, NULL, NULL }
};

static const plugin_action actions[] = {
	{ "list", core_list },
	{ "when", core_when },
	{ "set", core_set },
	{ "static", core_static },
	{ "test", core_test },
	{ NULL, NULL }
};

static const plugin_setup setups[] = {
	{ "listen", core_listen },
	{ NULL, NULL }
};

void plugin_core_init(server *srv, plugin *p) {
	UNUSED(srv);

	p->options = options;
	p->actions = actions;
	p->setups = setups;
}

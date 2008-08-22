
#include "base.h"
#include "plugin_core.h"
#include "utils.h"

static action* core_list(server *srv, plugin* p, option *opt) {
	action *a;
	guint i;
	UNUSED(p);

	if (!opt) {
		ERROR(srv, "%s", "need parameter");
		return NULL;
	}

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

	if (!opt) {
		ERROR(srv, "%s", "need parameter");
		return NULL;
	}
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

	if (!opt) {
		ERROR(srv, "%s", "need parameter");
		return NULL;
	}
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

static action_result core_handle_physical(server *srv, connection *con, gpointer param) {
	GString *docroot = (GString*) param;
	UNUSED(srv);

	if (con->state != CON_STATE_HANDLE_REQUEST_HEADER) return ACTION_GO_ON;

	g_string_truncate(con->physical.path, 0);
	g_string_append_len(con->physical.path, GSTR_LEN(docroot));
	g_string_append_len(con->physical.path, GSTR_LEN(con->request.uri.path));

	return ACTION_GO_ON;
}

static void core_physical_free(struct server *srv, gpointer param) {
	UNUSED(srv);
	g_string_free((GString*) param, TRUE);
}

static action* core_physical(server *srv, plugin* p, option *opt) {
	UNUSED(p);
	GString *docroot;

	if (!opt) {
		ERROR(srv, "%s", "need parameter");
		return NULL;
	}
	if (opt->type != OPTION_STRING) {
		ERROR(srv, "expected string as parameter, got %s", option_type_string(opt->type));
		return NULL;
	}

	docroot = (GString*) option_extract_value(opt);

	return action_new_function(core_handle_physical, core_physical_free, docroot);
}

static action_result core_handle_static(server *srv, connection *con, gpointer param) {
	UNUSED(param);
	int fd;

	if (con->state != CON_STATE_HANDLE_REQUEST_HEADER) return ACTION_GO_ON;

	if (con->physical.path->len == 0) return ACTION_GO_ON;

	fd = open(con->physical.path->str, O_RDONLY);
	if (fd == -1) {
		con->response.http_status = 404;
	} else {
		struct stat st;
		fstat(fd, &st);
#ifdef FD_CLOEXEC
		fcntl(fd, F_SETFD, FD_CLOEXEC);
#endif
		con->response.http_status = 200;
		chunkqueue_append_file_fd(con->out, NULL, 0, st.st_size, fd);
	}
	connection_handle_direct(srv, con);

	return ACTION_GO_ON;
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
	GHashTableIter iter;
	gpointer k, v;
	GList *hv;
	GString *str;
	gchar *backend;
	guint64 uptime;
	guint64 avg1, avg2, avg3;
	gchar suffix1[2] = {0,0}, suffix2[2] = {0,0}, suffix3[2] = {0,0};
	UNUSED(param);

	if (con->state != CON_STATE_HANDLE_REQUEST_HEADER) return ACTION_GO_ON;

	con->response.http_status = 200;
	chunkqueue_append_mem(con->out, CONST_STR_LEN("host: "));
	chunkqueue_append_mem(con->out, GSTR_LEN(con->request.uri.host));
	chunkqueue_append_mem(con->out, CONST_STR_LEN("\r\npath: "));
	chunkqueue_append_mem(con->out, GSTR_LEN(con->request.uri.path));
	chunkqueue_append_mem(con->out, CONST_STR_LEN("\r\nquery: "));
	chunkqueue_append_mem(con->out, GSTR_LEN(con->request.uri.query));

	chunkqueue_append_mem(con->out, CONST_STR_LEN("\r\n\r\nactions executed: "));
	uptime = (guint64)(ev_now(srv->loop) - srv->started);
	if (uptime == 0)
		uptime = 1;
	avg1 = srv->stats.actions_executed;
	suffix1[0] = counter_format(&avg1, 1000);
	avg2 = srv->stats.actions_executed / uptime;
	suffix2[0] = counter_format(&avg2, 1000);
	avg3 = srv->stats.actions_executed / srv->stats.requests;
	suffix3[0] = counter_format(&avg3, 1000);
	str = g_string_sized_new(0);
	g_string_printf(str,
		"%"G_GUINT64_FORMAT"%s (%"G_GUINT64_FORMAT"%s/s, %"G_GUINT64_FORMAT"%s/req)",
		avg1, suffix1, avg2, suffix2, avg3, suffix3
	);
	chunkqueue_append_string(con->out, str);
	chunkqueue_append_mem(con->out, CONST_STR_LEN("\r\nrequests: "));
	avg1 = srv->stats.requests;
	suffix1[0] = counter_format(&avg1, 1000);
	avg2 = srv->stats.requests / uptime;
	suffix2[0] = counter_format(&avg2, 1000);
	str = g_string_sized_new(0);
	g_string_printf(str, "%"G_GUINT64_FORMAT"%s (%"G_GUINT64_FORMAT"%s/s)", avg1, suffix1, avg2, suffix2);
	chunkqueue_append_string(con->out, str);

	backend = ev_backend_string(ev_backend(srv->loop));
	chunkqueue_append_mem(con->out, CONST_STR_LEN("\r\nevent handler: "));
	chunkqueue_append_mem(con->out, backend, strlen(backend));

	chunkqueue_append_mem(con->out, CONST_STR_LEN("\r\n\r\n--- headers ---\r\n"));
	g_hash_table_iter_init(&iter, con->request.headers->table);
	while (g_hash_table_iter_next(&iter, &k, &v)) {
		hv = g_queue_peek_head_link(&((http_header*)v)->values);
		while (hv != NULL) {
			chunkqueue_append_mem(con->out, GSTR_LEN(((http_header*)v)->key));
			chunkqueue_append_mem(con->out, CONST_STR_LEN(": "));
			chunkqueue_append_mem(con->out, GSTR_LEN((GString*)hv->data));
			chunkqueue_append_mem(con->out, CONST_STR_LEN("\r\n"));
			hv = hv->next;
		}
	}
	chunkqueue_append_mem(con->out, CONST_STR_LEN("\r\n"));
	connection_handle_direct(srv, con);

	log_debug(srv, con, "core_handle_test: %s%s%s log_level: %s",
		con->request.uri.path->str, con->request.uri.query->len ? "?" : "", con->request.uri.query->len ? con->request.uri.query->str : "",
		log_level_str((log_level_t)CORE_OPTION(CORE_OPTION_LOG_LEVEL))
	);

	return ACTION_GO_ON;
}

static action* core_test(server *srv, plugin* p, option *opt) {
	UNUSED(p);

	if (opt) {
		ERROR(srv, "%s", "'static' action doesn't have parameters");
		return NULL;
	}

	return action_new_function(core_handle_test, NULL, NULL);
}

static action_result core_handle_blank(server *srv, connection *con, gpointer param) {
	UNUSED(param);

	if (con->state != CON_STATE_HANDLE_REQUEST_HEADER) return ACTION_GO_ON;

	con->response.http_status = 200;
	connection_handle_direct(srv, con);

	return ACTION_GO_ON;
}

static action* core_blank(server *srv, plugin* p, option *opt) {
	UNUSED(p);

	if (opt) {
		ERROR(srv, "%s", "'empty' action doesn't have parameters");
		return NULL;
	}

	return action_new_function(core_handle_blank, NULL, NULL);
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


static gboolean core_event_handler(server *srv, plugin* p, option *opt) {
	guint backend;
	gchar *str;
	UNUSED(p);

	if (opt->type != OPTION_STRING) {
		ERROR(srv, "%s", "event_handler expects a string as parameter");
		return FALSE;
	}

	str = opt->value.opt_string->str;
	backend = 0; /* libev will chose the right one by default */

	if (g_str_equal(str, "select"))
		backend = EVBACKEND_SELECT;
	else if (g_str_equal(str, "poll"))
		backend = EVBACKEND_POLL;
	else if (g_str_equal(str, "epoll"))
		backend = EVBACKEND_EPOLL;
	else if (g_str_equal(str, "kqueue"))
		backend = EVBACKEND_KQUEUE;
	else if (g_str_equal(str, "devpoll"))
		backend = EVBACKEND_DEVPOLL;
	else if (g_str_equal(str, "port"))
		backend = EVBACKEND_PORT;
	else {
		ERROR(srv, "unkown event handler: '%s'", str);
		return FALSE;
	}

	if (backend) {
		if (!(ev_supported_backends() & backend)) {
			ERROR(srv, "unsupported event handler: '%s'", str);
			return FALSE;
		}

		if (!(ev_recommended_backends() & backend)) {
			TRACE(srv, "warning: event handler '%s' not recommended for this platform!", str);
		}
	}

	srv->loop_flags |= backend;

	return TRUE;
}


gboolean core_option_log_target_parse(server *srv, plugin *p, size_t ndx, option *opt, gpointer *value) {
	log_t *log;
	log_type_t log_type;

	UNUSED(ndx);
	UNUSED(p);

	assert(opt->type == OPTION_STRING);

	log_type = log_type_from_path(opt->value.opt_string);
	log = log_new(srv, log_type, opt->value.opt_string);

	*value = (gpointer)log;

	return TRUE;
}

void core_option_log_target_free(server *srv, plugin *p, size_t ndx, gpointer value) {
	UNUSED(srv);
	UNUSED(p);
	UNUSED(ndx);
	UNUSED(value);
}

gboolean core_option_log_level_parse(server *srv, plugin *p, size_t ndx, option *opt, gpointer *value) {
	UNUSED(srv);
	UNUSED(p);
	UNUSED(ndx);

	assert(opt->type == OPTION_STRING);

	*value = (gpointer)log_level_from_string(opt->value.opt_string);

	return TRUE;
}

void core_option_log_level_free(server *srv, plugin *p, size_t ndx, gpointer value) {
	UNUSED(srv);
	UNUSED(p);
	UNUSED(ndx);
	UNUSED(value);
}

gpointer core_option_max_keep_alive_idle_default(server *srv, plugin *p, gsize ndx) {
	UNUSED(srv);
	UNUSED(p);
	UNUSED(ndx);

	return GINT_TO_POINTER(5);
}

gpointer core_option_server_tag_default(server *srv, plugin *p, gsize ndx) {
	UNUSED(srv);
	UNUSED(p);
	UNUSED(ndx);

	return g_string_new_len(CONST_STR_LEN("lighttpd-2.0~sandbox"));
}

static const plugin_option options[] = {
	{ "debug.log_request_handling", OPTION_BOOLEAN, NULL, NULL, NULL },

	{ "log.target", OPTION_STRING, NULL, core_option_log_target_parse, core_option_log_target_free },
	{ "log.level", OPTION_STRING, NULL, core_option_log_level_parse, core_option_log_level_free },

	{ "static-file.exclude", OPTION_LIST, NULL, NULL, NULL },

	{ "server.tag", OPTION_STRING, core_option_server_tag_default, NULL, NULL },
	{ "server.max_keep_alive_idle", OPTION_INT, core_option_max_keep_alive_idle_default, NULL, NULL },
	{ NULL, 0, NULL, NULL, NULL }
};

static const plugin_action actions[] = {
	{ "list", core_list },
	{ "when", core_when },
	{ "set", core_set },

	{ "physical", core_physical },
	{ "static", core_static },
	{ "test", core_test },
	{ "blank", core_blank },
	{ NULL, NULL }
};

static const plugin_setup setups[] = {
	{ "listen", core_listen },
	{ "event_handler", core_event_handler },
	{ NULL, NULL }
};

void plugin_core_init(server *srv, plugin *p) {
	UNUSED(srv);

	p->options = options;
	p->actions = actions;
	p->setups = setups;
}

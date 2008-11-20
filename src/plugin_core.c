
#include <lighttpd/base.h>
#include <lighttpd/plugin_core.h>
#include <lighttpd/profiler.h>

#include <sys/stat.h>
#include <fcntl.h>

static action* core_list(server *srv, plugin* p, value *val) {
	action *a;
	guint i;
	UNUSED(p);

	if (!val) {
		ERROR(srv, "%s", "need parameter");
		return NULL;
	}

	if (val->type == VALUE_ACTION) {
		a = val->data.val_action.action;
		action_acquire(a);
		value_free(val);
		return a;
	}

	if (val->type != VALUE_LIST) {
		ERROR(srv, "expected list, got %s", value_type_string(val->type));
		return NULL;
	}

	a = action_new_list();
	for (i = 0; i < val->data.list->len; i++) {
		value *oa = g_array_index(val->data.list, value*, i);
		if (oa->type != VALUE_ACTION) {
			ERROR(srv, "expected action at entry %u of list, got %s", i, value_type_string(oa->type));
			action_release(srv, a);
			return NULL;
		}
		assert(srv == oa->data.val_action.srv);
		action_acquire(oa->data.val_action.action);
		g_array_append_val(a->data.list, oa->data.val_action.action);
	}
	return a;
}

static action* core_when(server *srv, plugin* p, value *val) {
	value *val_cond, *val_act, *val_act_else;
	action *a, *act_else;
	UNUSED(p);

	if (!val) {
		ERROR(srv, "%s", "need parameter");
		return NULL;
	}
	if (val->type != VALUE_LIST) {
		ERROR(srv, "expected list, got %s", value_type_string(val->type));
		return NULL;
	}
	if (val->data.list->len == 2) {
		val_act_else = NULL;
		act_else = NULL;
	} else if (val->data.list->len == 3) {
		val_act_else = g_array_index(val->data.list, value*, 2);
		act_else = val_act_else->data.val_action.action;
	} else {
		ERROR(srv, "expected list with length 2 or 3, has length %u", val->data.list->len);
		return NULL;
	}
	val_cond = g_array_index(val->data.list, value*, 0);
	val_act = g_array_index(val->data.list, value*, 1);

	if (val_cond->type != VALUE_CONDITION) {
		ERROR(srv, "expected condition as first parameter, got %s", value_type_string(val_cond->type));
		return NULL;
	}
	if (val_act->type != VALUE_ACTION) {
		ERROR(srv, "expected action as second parameter, got %s", value_type_string(val_act->type));
		return NULL;
	}
	if (val_act_else && val_act_else->type != VALUE_ACTION) {
		ERROR(srv, "expected action as third parameter, got %s", value_type_string(val_act_else->type));
		return NULL;
	}
	condition_acquire(val_cond->data.val_cond.cond);
	action_acquire(val_act->data.val_action.action);
	if (act_else) action_acquire(act_else);
	a = action_new_condition(val_cond->data.val_cond.cond, val_act->data.val_action.action, act_else);
	return a;
}

static action* core_set(server *srv, plugin* p, value *val) {
	value *val_val, *val_name;
	action *a;
	UNUSED(p);

	if (!val) {
		ERROR(srv, "%s", "need parameter");
		return NULL;
	}
	if (val->type != VALUE_LIST) {
		ERROR(srv, "expected list, got %s", value_type_string(val->type));
		return NULL;
	}
	if (val->data.list->len != 2) {
		ERROR(srv, "expected list with length 2, has length %u", val->data.list->len);
		return NULL;
	}
	val_name = g_array_index(val->data.list, value*, 0);
	val_val = g_array_index(val->data.list, value*, 1);
	if (val_name->type != VALUE_STRING) {
		ERROR(srv, "expected string as first parameter, got %s", value_type_string(val_name->type));
		return NULL;
	}
	a = option_action(srv, val_name->data.string->str, val_val);
	return a;
}

static gboolean core_setup_set(server *srv, plugin* p, value *val) {
	value *val_val, *val_name;
	UNUSED(p);

	if (!val) {
		ERROR(srv, "%s", "need parameter");
		return FALSE;
	}
	if (val->type != VALUE_LIST) {
		ERROR(srv, "expected list, got %s", value_type_string(val->type));
		return FALSE;
	}
	if (val->data.list->len != 2) {
		ERROR(srv, "expected list with length 2, has length %u", val->data.list->len);
		return FALSE;
	}
	val_name = g_array_index(val->data.list, value*, 0);
	val_val = g_array_index(val->data.list, value*, 1);
	if (val_name->type != VALUE_STRING) {
		ERROR(srv, "expected string as first parameter, got %s", value_type_string(val_name->type));
		return FALSE;
	}
	return plugin_set_default_option(srv, val_name->data.string->str, val_val);
}

static handler_t core_handle_static(vrequest *vr, gpointer param) {
	UNUSED(param);
	int fd;

	/* build physical path: docroot + uri.path */
	g_string_truncate(vr->physical.path, 0);
	g_string_append_len(vr->physical.path, GSTR_LEN(CORE_OPTION(CORE_OPTION_DOCROOT).string));
	g_string_append_len(vr->physical.path, GSTR_LEN(vr->request.uri.path));

	VR_TRACE(vr, "physical path: %s", vr->physical.path->str);

	if (vr->physical.path->len == 0) return HANDLER_GO_ON;

	if (!vrequest_handle_direct(vr)) return HANDLER_GO_ON;

	fd = open(vr->physical.path->str, O_RDONLY);
	if (fd == -1) {
		vr->response.http_status = 404;
		VR_TRACE(vr, "open() failed: %s (%d)\n", g_strerror(errno), errno);

		switch (errno) {
		case ENOENT:
			vr->response.http_status = 404; break;
		case EACCES:
		case EFAULT:
			vr->response.http_status = 403; break;
		default:
			vr->response.http_status = 500;
		}
	} else {
		struct stat st;
		fstat(fd, &st);

#ifdef FD_CLOEXEC
		fcntl(fd, F_SETFD, FD_CLOEXEC);
#endif
		if (!S_ISREG(st.st_mode)) {
			vr->response.http_status = 404;
			close(fd);
		} else {
			GString *mime_str = mimetype_get(vr, vr->request.uri.path);
			vr->response.http_status = 200;
			if (mime_str)
				http_header_overwrite(vr->response.headers, CONST_STR_LEN("Content-Type"), GSTR_LEN(mime_str));
			else
				http_header_overwrite(vr->response.headers, CONST_STR_LEN("Content-Type"), CONST_STR_LEN("application/octet-stream"));
			chunkqueue_append_file_fd(vr->out, NULL, 0, st.st_size, fd);
		}
	}

	return HANDLER_GO_ON;
}

static action* core_static(server *srv, plugin* p, value *val) {
	UNUSED(p);
	if (val) {
		ERROR(srv, "%s", "static action doesn't have parameters");
		return NULL;
	}

	return action_new_function(core_handle_static, NULL, NULL);
}

static handler_t core_handle_test(vrequest *vr, gpointer param) {
	connection *con = vr->con;
	server *srv = con->srv;
	worker *wrk = con->wrk;
	/*GHashTableIter iter;
	gpointer k, v;
	GList *hv;*/
	GString *str;
	gchar *backend;
	guint64 uptime;
	guint64 avg1, avg2, avg3;
	gchar suffix1[2] = {0,0}, suffix2[2] = {0,0}, suffix3[2] = {0,0};
	UNUSED(param);

	if (!vrequest_handle_direct(vr)) return HANDLER_GO_ON;

	vr->response.http_status = 200;
	chunkqueue_append_mem(vr->out, CONST_STR_LEN("host: "));
	chunkqueue_append_mem(vr->out, GSTR_LEN(vr->request.uri.host));
	chunkqueue_append_mem(vr->out, CONST_STR_LEN("\r\npath: "));
	chunkqueue_append_mem(vr->out, GSTR_LEN(vr->request.uri.path));
	chunkqueue_append_mem(vr->out, CONST_STR_LEN("\r\nquery: "));
	chunkqueue_append_mem(vr->out, GSTR_LEN(vr->request.uri.query));

	chunkqueue_append_mem(vr->out, CONST_STR_LEN("\r\n\r\nactions executed: "));
	uptime = (guint64)(ev_now(con->wrk->loop) - srv->started);
	if (uptime == 0)
		uptime = 1;
	avg1 = wrk->stats.actions_executed;
	suffix1[0] = counter_format(&avg1, 1000);
	avg2 = wrk->stats.actions_executed / uptime;
	suffix2[0] = counter_format(&avg2, 1000);
	avg3 = wrk->stats.actions_executed / wrk->stats.requests;
	suffix3[0] = counter_format(&avg3, 1000);
	str = g_string_sized_new(0);
	g_string_printf(str,
		"%"G_GUINT64_FORMAT"%s (%"G_GUINT64_FORMAT"%s/s, %"G_GUINT64_FORMAT"%s/req)",
		avg1, suffix1, avg2, suffix2, avg3, suffix3
	);
	chunkqueue_append_string(vr->out, str);
	chunkqueue_append_mem(vr->out, CONST_STR_LEN("\r\nrequests: "));
	avg1 = wrk->stats.requests;
	suffix1[0] = counter_format(&avg1, 1000);
	avg2 = wrk->stats.requests / uptime;
	suffix2[0] = counter_format(&avg2, 1000);
	str = g_string_sized_new(0);
	g_string_printf(str, "%"G_GUINT64_FORMAT"%s (%"G_GUINT64_FORMAT"%s/s)", avg1, suffix1, avg2, suffix2);
	chunkqueue_append_string(vr->out, str);

	backend = ev_backend_string(ev_backend(con->wrk->loop));
	chunkqueue_append_mem(vr->out, CONST_STR_LEN("\r\nevent handler: "));
	chunkqueue_append_mem(vr->out, backend, strlen(backend));

/*	chunkqueue_append_mem(vr->out, CONST_STR_LEN("\r\n\r\n--- headers ---\r\n"));
	g_hash_table_iter_init(&iter, con->request.headers->table);
	while (g_hash_table_iter_next(&iter, &k, &v)) {
		hv = g_queue_peek_head_link(&((http_header*)v)->values);
		while (hv != NULL) {
			chunkqueue_append_mem(vr->out, GSTR_LEN(((http_header*)v)->key));
			chunkqueue_append_mem(vr->out, CONST_STR_LEN(": "));
			chunkqueue_append_mem(vr->out, GSTR_LEN((GString*)hv->data));
			chunkqueue_append_mem(vr->out, CONST_STR_LEN("\r\n"));
			hv = hv->next;
		}
	}*/
	chunkqueue_append_mem(vr->out, CONST_STR_LEN("\r\n"));

	return HANDLER_GO_ON;
}

static action* core_test(server *srv, plugin* p, value *val) {
	UNUSED(p);

	if (val) {
		ERROR(srv, "%s", "'static' action doesn't have parameters");
		return NULL;
	}

	return action_new_function(core_handle_test, NULL, NULL);
}

static handler_t core_handle_blank(vrequest *vr, gpointer param) {
	UNUSED(param);

	if (!vrequest_handle_direct(vr)) return HANDLER_GO_ON;

	vr->response.http_status = 200;

	return HANDLER_GO_ON;
}

static action* core_blank(server *srv, plugin* p, value *val) {
	UNUSED(p);

	if (val) {
		ERROR(srv, "%s", "'empty' action doesn't have parameters");
		return NULL;
	}

	return action_new_function(core_handle_blank, NULL, NULL);
}

static handler_t core_handle_profile_mem(vrequest *vr, gpointer param) {
	UNUSED(vr);
	UNUSED(param);

	/*g_mem_profile();*/
	profiler_dump();

	return HANDLER_GO_ON;
}

static action* core_profile_mem(server *srv, plugin* p, value *val) {
	UNUSED(p);

	if (val) {
		ERROR(srv, "%s", "'profile_mem' action doesn't have parameters");
		return NULL;
	}

	return action_new_function(core_handle_profile_mem, NULL, NULL);
}

static gboolean core_listen(server *srv, plugin* p, value *val) {
	guint32 ipv4;
#ifdef HAVE_IPV6
	guint8 ipv6[16];
#endif
	GString *ipstr;
	guint16 port = 80;
	UNUSED(p);

	if (val->type != VALUE_STRING) {
		ERROR(srv, "%s", "listen expects a string as parameter");
		return FALSE;
	}

	if (val->data.string->str[0] == '[') {
		/* ipv6 with port */
		gchar *pos = g_strrstr(val->data.string->str, "]");
		if (NULL == pos) {
			ERROR(srv, "%s", "listen: bogus ipv6 format");
			return FALSE;
		}
		if (pos[1] == ':') {
			port = atoi(&pos[2]);
		}
		ipstr = g_string_new_len(&val->data.string->str[1], pos - &val->data.string->str[1]);
	} else {
		/* no brackets, search for :port */
		gchar *pos = g_strrstr(val->data.string->str, ":");
		if (NULL != pos) {
			ipstr = g_string_new_len(val->data.string->str, pos - val->data.string->str);
			port = atoi(&pos[1]);
		} else {
			/* no port, just plain ipv4 or ipv6 address */
			ipstr = g_string_new_len(GSTR_LEN(val->data.string));
		}
	}

	if (parse_ipv4(ipstr->str, &ipv4, NULL)) {
		int s, v;
		struct sockaddr_in addr;
		memset(&addr, 0, sizeof(addr));
		addr.sin_family = AF_INET;
		addr.sin_addr.s_addr = ipv4;
		addr.sin_port = htons(port);
		g_string_free(ipstr, TRUE);
		if (-1 == (s = socket(AF_INET, SOCK_STREAM, 0))) {
			ERROR(srv, "Couldn't open socket: %s", g_strerror(errno));
			return FALSE;
		}
		v = 1;
		if (-1 == setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &v, sizeof(v))) {
			close(s);
			ERROR(srv, "Couldn't setsockopt(SO_REUSEADDR) to '%s': %s", inet_ntoa(*(struct in_addr*)&ipv4), g_strerror(errno));
			return FALSE;
		}
		if (-1 == bind(s, (struct sockaddr*)&addr, sizeof(addr))) {
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
		TRACE(srv, "listen to ipv4: '%s' port: %d", inet_ntoa(*(struct in_addr*)&ipv4), port);
#ifdef HAVE_IPV6
	} else if (parse_ipv6(ipstr->str, ipv6, NULL)) {
		/* TODO: IPv6 */
		g_string_free(ipstr, TRUE);
		ERROR(srv, "%s", "IPv6 not supported yet");
		return FALSE;
#endif
	} else {
		ERROR(srv, "Invalid ip: '%s'", ipstr->str);
		g_string_free(ipstr, TRUE);
		return FALSE;
	}

	return TRUE;
}


static gboolean core_event_handler(server *srv, plugin* p, value *val) {
	guint backend;
	gchar *str;
	UNUSED(p);

	if (val->type != VALUE_STRING) {
		ERROR(srv, "%s", "event_handler expects a string as parameter");
		return FALSE;
	}

	str = val->data.string->str;
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

static gboolean core_workers(server *srv, plugin* p, value *val) {
	gint workers;
	UNUSED(p);

	workers = val->data.number;
	if (val->type != VALUE_NUMBER || workers < 1) {
		ERROR(srv, "%s", "workers expects a positive integer as parameter");
		return FALSE;
	}

	if (srv->worker_count != 0) {
		ERROR(srv, "workers already called with '%i', overwriting", srv->worker_count);
	}
	srv->worker_count = workers;
	return TRUE;
}

static gboolean core_module_load(server *srv, plugin* p, value *val) {
	value *mods = value_new_list();
	UNUSED(p);

	if (!g_module_supported()) {
		ERROR(srv, "%s", "module loading not supported on this platform");
		value_free(mods);
		return FALSE;
	}

	if (val->type == VALUE_STRING) {
		/* load only one module */
		value *name = value_new_string(value_extract(val).string);
		g_array_append_val(mods->data.list, name);
	} else if (val->type == VALUE_LIST) {
		/* load a list of modules */
		for (guint i = 0; i < val->data.list->len; i++) {
			value *v = g_array_index(val->data.list, value*, i);
			if (v->type != VALUE_STRING) {
				ERROR(srv, "module_load takes either a string or a list of strings as parameter, list with %s entry given", value_type_string(v->type));
				value_free(mods);
				return FALSE;
			}
		}
		mods->data.list = value_extract(val).list;
	} else {
		ERROR(srv, "module_load takes either a string or a list of strings as parameter, %s given", value_type_string(val->type));
		return FALSE;
	}

	/* parameter types ok, load modules */
	for (guint i = 0; i < mods->data.list->len; i++) {
		GString *name = g_array_index(mods->data.list, value*, i)->data.string;
		if (!module_load(srv->modules, name->str)) {
			ERROR(srv, "could not load module '%s': %s", name->str, g_module_error());
			value_free(mods);
			return FALSE;
		}

		TRACE(srv, "loaded module '%s'", name->str);
	}

	value_free(mods);

	return TRUE;
}

static gboolean core_io_timeout(server *srv, plugin* p, value *val) {
	UNUSED(p);

	if (!val || val->type != VALUE_NUMBER || val->data.number < 1) {
		ERROR(srv, "%s", "io_timeout expects a positive number as parameter");
		return FALSE;
	}

	srv->io_timeout = value_extract(val).number;

	return TRUE;
}

/*
 * OPTIONS
 */

static gboolean core_option_log_parse(server *srv, plugin *p, size_t ndx, value *val, option_value *oval) {
	GHashTableIter iter;
	gpointer k, v;
	log_level_t level;
	GString *path;
	GString *level_str;
	GArray *arr = g_array_sized_new(FALSE, TRUE, sizeof(log_t*), 5);
	UNUSED(p);
	UNUSED(ndx);

	oval->list = arr;
	g_array_set_size(arr, 5);

	/* default value */
	if (!val) {
		/* default: log LOG_LEVEL_WARNING and LOG_LEVEL_ERROR to stderr */
		log_t *log = srv->logs.stderr;
		log_ref(srv, log);
		g_array_index(arr, log_t*, LOG_LEVEL_WARNING) = log;
		log_ref(srv, log);
		g_array_index(arr, log_t*, LOG_LEVEL_ERROR) = log;
		return TRUE;
	}

	g_hash_table_iter_init(&iter, val->data.hash);
	while (g_hash_table_iter_next(&iter, &k, &v)) {
		if (((value*)v)->type != VALUE_STRING) {
			ERROR(srv, "log expects a hashtable with string values, %s given", value_type_string(((value*)v)->type));
			g_array_free(arr, TRUE);
			return FALSE;
		}

		path = ((value*)v)->data.string;
		level_str = (GString*)k;

		if (g_str_equal(level_str->str, "*")) {
			for (guint i = 0; i < arr->len; i++) {
				if (NULL != g_array_index(arr, log_t*, i))
					continue;
				log_t *log = log_new(srv, log_type_from_path(path), path);
				g_array_index(arr, log_t*, i) = log;
			}
		}
		else {
			log_t *log = log_new(srv, log_type_from_path(path), path);
			level = log_level_from_string(level_str);
			g_array_index(arr, log_t*, level) = log;
		}
	}

	return TRUE;
}

static void core_option_log_free(server *srv, plugin *p, size_t ndx, option_value oval) {
	UNUSED(p);
	UNUSED(ndx);

	GArray *arr = oval.list;
	if (!arr) return;

	for (guint i = 0; i < arr->len; i++) {
		if (NULL != g_array_index(arr, log_t*, i))
			log_unref(srv, g_array_index(arr, log_t*, i));
	}
	g_array_free(arr, TRUE);
}

static gboolean core_option_log_timestamp_parse(server *srv, plugin *p, size_t ndx, value *val, option_value *oval) {
	UNUSED(p);
	UNUSED(ndx);

	if (!val) return TRUE;
	oval->ptr = log_timestamp_new(srv, value_extract(val).string);

	return TRUE;
}

static void core_option_log_timestamp_free(server *srv, plugin *p, size_t ndx, option_value oval) {
	UNUSED(p);
	UNUSED(ndx);

	if (!oval.ptr) return;
	log_timestamp_free(srv, oval.ptr);
}

static gboolean core_option_mime_types_parse(server *srv, plugin *p, size_t ndx, value *val, option_value *oval) {
	UNUSED(srv);
	UNUSED(p);
	UNUSED(ndx);
	GArray *arr;


	/* default value */
	if (!val) {
		oval->list = g_array_new(FALSE, TRUE, sizeof(value));
		return TRUE;
	}

	/* check if the passed val is of type (("a", "b"), ("x", y")) */
	arr = val->data.list;
	for (guint i = 0; i < arr->len; i++) {
		value *v = g_array_index(arr, value*, i);
		value *v1, *v2;
		if (v->type != VALUE_LIST) {
			ERROR(srv, "mime_types option expects a list of string tuples, entry #%u is of type %s", i, value_type_string(v->type));
			return FALSE;
		}

		if (v->data.list->len != 2) {
			ERROR(srv, "mime_types option expects a list of string tuples, entry #%u is not a tuple", i);
			return FALSE;
		}

		v1 = g_array_index(v->data.list, value*, 0);
		v2 = g_array_index(v->data.list, value*, 1);
		if (v1->type != VALUE_STRING || v2->type != VALUE_STRING) {
			ERROR(srv, "mime_types option expects a list of string tuples, entry #%u is a (%s,%s) tuple", i, value_type_string(v1->type), value_type_string(v2->type));
			return FALSE;
		}
	}

	/* everything ok */
	oval->list = value_extract(val).list;

	return TRUE;
}

static void core_option_mime_types_free(server *srv, plugin *p, size_t ndx, option_value oval) {
	UNUSED(srv);
	UNUSED(p);
	UNUSED(ndx);

	for (guint i = 0; i < oval.list->len; i++)
		value_free(g_array_index(oval.list, value*, i));

	g_array_free(oval.list, TRUE);
}

static handler_t core_handle_header_add(vrequest *vr, gpointer param) {
	UNUSED(param);
	GArray *l = (GArray*)param;
	GString *k = g_array_index(l, value*, 0)->data.string;
	GString *v = g_array_index(l, value*, 1)->data.string;

	http_header_insert(vr->response.headers, GSTR_LEN(k), GSTR_LEN(v));

	return HANDLER_GO_ON;
}

static void core_header_free(struct server *srv, gpointer param) {
	UNUSED(srv);
	value_list_free(param);
}

static action* core_header_add(server *srv, plugin* p, value *val) {
	GArray *l;
	UNUSED(p);

	if (val->type != VALUE_LIST) {
		ERROR(srv, "'core_header_add' action expects a string tuple as parameter, %s given", value_type_string(val->type));
		return NULL;
	}

	l = val->data.list;

	if (l->len != 2) {
		ERROR(srv, "'core_header_add' action expects a string tuple as parameter, list has %u entries", l->len);
		return NULL;
	}

	if (g_array_index(l, value*, 0)->type != VALUE_STRING || g_array_index(l, value*, 0)->type != VALUE_STRING) {
		ERROR(srv, "%s", "'core_header_add' action expects a string tuple as parameter");
		return NULL;
	}

	return action_new_function(core_handle_header_add, core_header_free, value_extract(val).list);
}


static handler_t core_handle_header_append(vrequest *vr, gpointer param) {
	UNUSED(param);
	GArray *l = (GArray*)param;
	GString *k = g_array_index(l, value*, 0)->data.string;
	GString *v = g_array_index(l, value*, 1)->data.string;

	http_header_append(vr->response.headers, GSTR_LEN(k), GSTR_LEN(v));

	return HANDLER_GO_ON;
}

static action* core_header_append(server *srv, plugin* p, value *val) {
	GArray *l;
	UNUSED(p);

	if (val->type != VALUE_LIST) {
		ERROR(srv, "'core_header_append' action expects a string tuple as parameter, %s given", value_type_string(val->type));
		return NULL;
	}

	l = val->data.list;

	if (l->len != 2) {
		ERROR(srv, "'core_header_append' action expects a string tuple as parameter, list has %u entries", l->len);
		return NULL;
	}

	if (g_array_index(l, value*, 0)->type != VALUE_STRING || g_array_index(l, value*, 0)->type != VALUE_STRING) {
		ERROR(srv, "%s", "'core_header_append' action expects a string tuple as parameter");
		return NULL;
	}

	return action_new_function(core_handle_header_append, core_header_free, value_extract(val).list);
}


static handler_t core_handle_header_overwrite(vrequest *vr, gpointer param) {
	UNUSED(param);
	GArray *l = (GArray*)param;
	GString *k = g_array_index(l, value*, 0)->data.string;
	GString *v = g_array_index(l, value*, 1)->data.string;

	http_header_overwrite(vr->response.headers, GSTR_LEN(k), GSTR_LEN(v));

	return HANDLER_GO_ON;
}

static action* core_header_overwrite(server *srv, plugin* p, value *val) {
	GArray *l;
	UNUSED(p);

	if (val->type != VALUE_LIST) {
		ERROR(srv, "'core_header_overwrite' action expects a string tuple as parameter, %s given", value_type_string(val->type));
		return NULL;
	}

	l = val->data.list;

	if (l->len != 2) {
		ERROR(srv, "'core_header_overwrite' action expects a string tuple as parameter, list has %u entries", l->len);
		return NULL;
	}

	if (g_array_index(l, value*, 0)->type != VALUE_STRING || g_array_index(l, value*, 0)->type != VALUE_STRING) {
		ERROR(srv, "%s", "'core_header_overwrite' action expects a string tuple as parameter");
		return NULL;
	}

	return action_new_function(core_handle_header_overwrite, core_header_free, value_extract(val).list);
}


static const plugin_option options[] = {
	{ "debug.log_request_handling", VALUE_BOOLEAN, NULL, NULL, NULL },

	{ "log.timestamp", VALUE_STRING, NULL, core_option_log_timestamp_parse, core_option_log_timestamp_free },
	{ "log", VALUE_HASH, NULL, core_option_log_parse, core_option_log_free },

	{ "static-file.exclude", VALUE_LIST, NULL, NULL, NULL },

	{ "server.tag", VALUE_STRING, "lighttpd-2.0~sandbox", NULL, NULL },
	{ "server.max_keep_alive_idle", VALUE_NUMBER, GINT_TO_POINTER(5), NULL, NULL },

	{ "mime_types", VALUE_LIST, NULL, core_option_mime_types_parse, core_option_mime_types_free },

	{ "docroot", VALUE_STRING, NULL, NULL, NULL },

	{ "throttle", VALUE_NUMBER, GINT_TO_POINTER(0), NULL, NULL },

	{ NULL, 0, NULL, NULL, NULL }
};

static const plugin_action actions[] = {
	{ "list", core_list },
	{ "when", core_when },
	{ "set", core_set },

	{ "static", core_static },

	{ "test", core_test },
	{ "blank", core_blank },
	{ "profile_mem", core_profile_mem },

	{ "header_add", core_header_add },
	{ "header_append", core_header_append },
	{ "header_overwrite", core_header_overwrite },

	{ NULL, NULL }
};

static const plugin_setup setups[] = {
	{ "set_default", core_setup_set },
	{ "listen", core_listen },
	{ "event_handler", core_event_handler },
	{ "workers", core_workers },
	{ "module_load", core_module_load },
	{ "io_timeout", core_io_timeout },
	{ NULL, NULL }
};

void plugin_core_init(server *srv, plugin *p) {
	UNUSED(srv);

	p->options = options;
	p->actions = actions;
	p->setups = setups;
}


#include <lighttpd/angel_plugin_core.h>
#include <lighttpd/ip_parsers.h>

#include <pwd.h>
#include <grp.h>

static void core_instance_parse(server *srv, plugin *p, value **options) {
	GPtrArray *cmd;
	gchar **cmdarr;
	plugin_core_config_t *config = (plugin_core_config_t*) p->data;
	uid_t uid = -1;
	gid_t gid = -1;
	GString *user = NULL;

	if (config->load_instconf) {
		ERROR(srv, "%s", "Already configure the instance");
		config->load_failed = FALSE;
		return;
	}

	/* set user and group */
	if (options[0]) {
		struct passwd *pwd;
		user = options[0]->data.string;
		if (NULL == (pwd = getpwnam(user->str))) {
			ERROR(srv, "can't find username '%s'", user->str);
			config->load_failed = FALSE;
			return;
		}

		uid = pwd->pw_uid;
		gid = pwd->pw_gid;
	}

	if (options[1]) {
		struct group *grp;
		GString *group = options[1]->data.string;
		if (NULL == (grp = getgrnam(group->str))) {
			ERROR(srv, "can't find groupname '%s'", group->str);
			config->load_failed = FALSE;
			return;
		}

		gid = grp->gr_gid;
	}

	if (0 == uid) {
		ERROR(srv, "%s", "I will not set uid to 0");
		config->load_failed = FALSE;
		return;
	}
	if (0 == gid) {
		ERROR(srv, "%s", "I will not set gid to 0");
		config->load_failed = FALSE;
		return;
	}

	cmd = g_ptr_array_new();
#if 0
	g_ptr_array_add(cmd, g_strndup(CONST_STR_LEN("/usr/bin/valgrind")));
#endif
	if (options[2]) {
		g_ptr_array_add(cmd, g_strndup(GSTR_LEN(options[2]->data.string)));
	} else {
		g_ptr_array_add(cmd, g_strndup(CONST_STR_LEN("/usr/bin/lighttpd")));
	}

	g_ptr_array_add(cmd, g_strndup(CONST_STR_LEN("--angel")));
	g_ptr_array_add(cmd, g_strndup(CONST_STR_LEN("-c")));
	if (options[3]) {
		g_ptr_array_add(cmd, g_strndup(GSTR_LEN(options[3]->data.string)));
	} else if (options[4]) {
		g_ptr_array_add(cmd, g_strndup(GSTR_LEN(options[4]->data.string)));
		g_ptr_array_add(cmd, g_strndup(CONST_STR_LEN("-l")));
	} else {
		g_ptr_array_add(cmd, g_strndup(CONST_STR_LEN("/etc/lighttpd2/lighttpd.conf")));
	}

	g_ptr_array_add(cmd, g_strndup(CONST_STR_LEN("-m")));
	if (options[5]) {
		g_ptr_array_add(cmd, g_strndup(GSTR_LEN(options[5]->data.string)));
	} else {
		g_ptr_array_add(cmd, g_strndup(CONST_STR_LEN("/usr/lib/lighttpd2/")));
	}

	g_ptr_array_add(cmd, NULL);
	cmdarr = (gchar**) g_ptr_array_free(cmd, FALSE);
	config->load_instconf = instance_conf_new(cmdarr, user, uid, gid);
}

static const plugin_item_option core_instance_options[] = {
	{ "user", VALUE_STRING, 0 },
	{ "group", VALUE_STRING, 0 },
	{ "binary", VALUE_STRING, 0 },
	{ "config", VALUE_STRING, 0 },
	{ "luaconfig", VALUE_STRING, 0 },
	{ "modules", VALUE_STRING, 0 },
	{ NULL, 0, 0 }
};

static const plugin_item core_items[] = {
	{ "instance", core_instance_parse, core_instance_options },
	{ NULL, NULL, NULL }
};

static int do_listen(server *srv, GString *str) {
	guint32 ipv4;
#ifdef HAVE_IPV6
	guint8 ipv6[16];
#endif
	guint16 port = 80;

	if (parse_ipv4(str->str, &ipv4, NULL, &port)) {
		int s, v;
		struct sockaddr_in addr;
		memset(&addr, 0, sizeof(addr));
		addr.sin_family = AF_INET;
		addr.sin_addr.s_addr = ipv4;
		addr.sin_port = htons(port);
		if (-1 == (s = socket(AF_INET, SOCK_STREAM, 0))) {
			ERROR(srv, "Couldn't open socket: %s", g_strerror(errno));
			return -1;
		}
		v = 1;
		if (-1 == setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &v, sizeof(v))) {
			close(s);
			ERROR(srv, "Couldn't setsockopt(SO_REUSEADDR): %s", g_strerror(errno));
			return -1;
		}
		if (-1 == bind(s, (struct sockaddr*)&addr, sizeof(addr))) {
			close(s);
			ERROR(srv, "Couldn't bind socket to '%s': %s", str->str, g_strerror(errno));
			return -1;
		}
		if (-1 == listen(s, 1000)) {
			close(s);
			ERROR(srv, "Couldn't listen on '%s': %s", str->str, g_strerror(errno));
			return -1;
		}
		DEBUG(srv, "listen to ipv4: '%s' port: %d", str->str, port);
		return s;
#ifdef HAVE_IPV6
	} else if (parse_ipv6(str->str, ipv6, NULL, &port)) {
		GString *ipv6_str = g_string_sized_new(0);
		int s, v;
		struct sockaddr_in6 addr;
		ipv6_tostring(ipv6_str, ipv6);

		memset(&addr, 0, sizeof(addr));
		addr.sin6_family = AF_INET6;
		memcpy(&addr.sin6_addr, ipv6, 16);
		addr.sin6_port = htons(port);
		if (-1 == (s = socket(AF_INET6, SOCK_STREAM, 0))) {
			ERROR(srv, "Couldn't open socket: %s", g_strerror(errno));
			g_string_free(ipv6_str, TRUE);
			return -1;
		}
		v = 1;
		if (-1 == setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &v, sizeof(v))) {
			close(s);
			ERROR(srv, "Couldn't setsockopt(SO_REUSEADDR): %s", g_strerror(errno));
			g_string_free(ipv6_str, TRUE);
			return -1;
		}
		if (-1 == setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, &v, sizeof(v))) {
			close(s);
			ERROR(srv, "Couldn't setsockopt(IPV6_V6ONLY): %s", g_strerror(errno));
			g_string_free(ipv6_str, TRUE);
			return -1;
		}
		if (-1 == bind(s, (struct sockaddr*)&addr, sizeof(addr))) {
			close(s);
			ERROR(srv, "Couldn't bind socket to '%s': %s", ipv6_str->str, g_strerror(errno));
			g_string_free(ipv6_str, TRUE);
			return -1;
		}
		if (-1 == listen(s, 1000)) {
			close(s);
			ERROR(srv, "Couldn't listen on '%s': %s", ipv6_str->str, g_strerror(errno));
			g_string_free(ipv6_str, TRUE);
			return -1;
		}
		DEBUG(srv, "listen to ipv6: '%s' port: %d", ipv6_str->str, port);
		g_string_free(ipv6_str, TRUE);
		return s;
#endif
	} else {
		ERROR(srv, "Invalid ip: '%s'", str->str);
		return -1;
	}
}

static void core_listen(server *srv, instance *i, plugin *p, gint32 id, GString *data) {
	GError *err = NULL;
	gint fd;
	GArray *fds;
	DEBUG(srv, "core_listen(%i) '%s'", id, data->str);

	if (-1 == id) return; /* ignore simple calls */

	fd = do_listen(srv, data);

	if (-1 == fd) {
		GString *error = g_string_sized_new(0);
		g_string_printf(error, "Couldn't listen to '%s'", data->str);
		if (!angel_send_result(i->acon, id, error, NULL, NULL, &err)) {
			ERROR(srv, "Couldn't send result: %s", err->message);
			g_error_free(err);
		}
		return;
	}

	fds = g_array_new(FALSE, FALSE, sizeof(int));
	g_array_append_val(fds, fd);

	if (!angel_send_result(i->acon, id, NULL, NULL, fds, &err)) {
		ERROR(srv, "Couldn't send result: %s", err->message);
		g_error_free(err);
		return;
	}
}

static void core_clean(server *srv, plugin *p);
static void core_free(server *srv, plugin *p) {
	plugin_core_config_t *config = (plugin_core_config_t*) p->data;

	core_clean(srv, p);

	if (config->instconf) {
		instance_conf_release(config->instconf);
		config->instconf = NULL;
	}

	if (config->inst) {
		instance_set_state(config->inst, INSTANCE_DOWN);
		instance_release(config->inst);
		config->inst = NULL;
	}
}

static void core_clean(server *srv, plugin *p) {
	plugin_core_config_t *config = (plugin_core_config_t*) p->data;
	UNUSED(srv);

	if (config->load_instconf) {
		instance_conf_release(config->load_instconf);
		config->load_instconf = NULL;
	}

	config->load_failed = FALSE;
}

static gboolean core_check(server *srv, plugin *p) {
	plugin_core_config_t *config = (plugin_core_config_t*) p->data;
	UNUSED(srv);
	return !config->load_failed;
}

static void core_activate(server *srv, plugin *p) {
	plugin_core_config_t *config = (plugin_core_config_t*) p->data;

	if (config->instconf) {
		instance_conf_release(config->instconf);
		config->instconf = NULL;
	}

	if (config->inst) {
		instance_set_state(config->inst, INSTANCE_DOWN);
		instance_release(config->inst);
		config->inst = NULL;
	}

	config->instconf = config->load_instconf;
	config->load_instconf = NULL;

	if (config->instconf) {
		config->inst = server_new_instance(srv, config->instconf);
		instance_set_state(config->inst, INSTANCE_ACTIVE);
		ERROR(srv, "%s", "Starting instance");
	}
}

static gboolean core_init(server *srv, plugin *p) {
	UNUSED(srv);
	p->data = g_slice_new0(plugin_core_config_t);
	p->items = core_items;

	p->handle_free = core_free;
	p->handle_clean_config = core_clean;
	p->handle_check_config = core_check;
	p->handle_activate_config = core_activate;

	g_hash_table_insert(p->angel_callbacks, "listen", (gpointer)(intptr_t)core_listen);

	return TRUE;
}

gboolean plugin_core_init(server *srv) {
	/* load core plugins */
	return NULL != angel_plugin_register(srv, NULL, "core", core_init);
}

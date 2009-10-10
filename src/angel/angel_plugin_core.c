
#include <lighttpd/angel_plugin_core.h>
#include <lighttpd/ip_parsers.h>

#include <pwd.h>
#include <grp.h>

static void core_instance_parse(liServer *srv, liPlugin *p, liValue **options) {
	GPtrArray *cmd, *env;
	gchar **cmdarr, **envarr;
	liPluginCoreConfig *config = (liPluginCoreConfig*) p->data;
	uid_t uid = -1;
	gid_t gid = -1;
	GString *user = NULL;
	gint64 rlim_core = -1, rlim_nofile = -1;

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

	/* check types in lists */
	if (options[6]) {
		GPtrArray *wrapper_lst = options[6]->data.list;
		guint i;
		for (i = 0; i < wrapper_lst->len; i++) {
			liValue *wi = g_ptr_array_index(wrapper_lst, i);
			if (wi->type != LI_VALUE_STRING) {
				ERROR(srv, "Entry %i in wrapper list is not a string", i);
				config->load_failed = FALSE;
				return;
			}
		}
	}
	if (options[7]) { /* env */
		GPtrArray *env_lst = options[7]->data.list;
		guint i;
		for (i = 0; i < env_lst->len; i++) {
			liValue *ei = g_ptr_array_index(env_lst, i);
			if (ei->type != LI_VALUE_STRING) {
				ERROR(srv, "Entry %i in env list is not a string", i);
				config->load_failed = FALSE;
				return;
			}
		}
	}
	if (options[8]) { /* copy-env */
		GPtrArray *env_lst = options[8]->data.list;
		guint i;
		for (i = 0; i < env_lst->len; i++) {
			liValue *ei = g_ptr_array_index(env_lst, i);
			if (ei->type != LI_VALUE_STRING) {
				ERROR(srv, "Entry %i in copy-env list is not a string", i);
				config->load_failed = FALSE;
				return;
			}
		}
	}

	env = g_ptr_array_new();

	if (options[7]) { /* env */
		GPtrArray *env_lst = options[7]->data.list;
		guint i;
		for (i = 0; i < env_lst->len; i++) {
			liValue *ei = g_ptr_array_index(env_lst, i);
			g_ptr_array_add(env, g_strndup(GSTR_LEN(ei->data.string)));
		}
	}
	if (options[8]) { /* copy-env */
		GPtrArray *env_lst = options[8]->data.list;
		guint i;
		for (i = 0; i < env_lst->len; i++) {
			liValue *ei = g_ptr_array_index(env_lst, i);
			const char *val = getenv(ei->data.string->str);
			size_t vallen, keylen = ei->data.string->len;
			gchar *entry;
			if (!val) continue;

			vallen = strlen(val);
			entry = g_malloc(keylen + 1 /* "=" */ + vallen + 1 /* \0 */);
			memcpy(entry, ei->data.string->str, keylen);
			entry[keylen] = '=';
			memcpy(entry + keylen+1, val, vallen);
			entry[keylen + vallen + 1] = '\0';
			g_ptr_array_add(env, entry);
		}
	}


	cmd = g_ptr_array_new();

	if (options[6]) {
		GPtrArray *wrapper_lst = options[6]->data.list;
		guint i;
		for (i = 0; i < wrapper_lst->len; i++) {
			liValue *wi = g_ptr_array_index(wrapper_lst, i);
			g_ptr_array_add(cmd, g_strndup(GSTR_LEN(wi->data.string)));
		}
	}

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

	if (options[9]) rlim_core = options[9]->data.number;
	if (options[10]) rlim_nofile = options[10]->data.number;

	g_ptr_array_add(cmd, NULL);
	cmdarr = (gchar**) g_ptr_array_free(cmd, FALSE);
	envarr = (gchar**) g_ptr_array_free(env, FALSE);
	config->load_instconf = li_instance_conf_new(cmdarr, envarr, user, uid, gid, rlim_core, rlim_nofile);
}

static const liPluginItemOption core_instance_options[] = {
	/*  0 */ { "user", LI_VALUE_STRING, 0 },
	/*  1 */ { "group", LI_VALUE_STRING, 0 },
	/*  2 */ { "binary", LI_VALUE_STRING, 0 },
	/*  3 */ { "config", LI_VALUE_STRING, 0 },
	/*  4 */ { "luaconfig", LI_VALUE_STRING, 0 },
	/*  5 */ { "modules", LI_VALUE_STRING, 0 },
	/*  6 */ { "wrapper", LI_VALUE_LIST, 0 },
	/*  7 */ { "env", LI_VALUE_LIST, 0 },
	/*  8 */ { "copy-env", LI_VALUE_LIST, 0 },
	/*  9 */ { "max-core-file-size", LI_VALUE_NUMBER, 0 },
	/* 10 */ { "max-open-files", LI_VALUE_NUMBER, 0 },
	{ NULL, 0, 0 }
};

static void core_listen_mask_free(liPluginCoreListenMask *mask) {
	switch (mask->type) {
	case LI_PLUGIN_CORE_LISTEN_MASK_IPV4:
	case LI_PLUGIN_CORE_LISTEN_MASK_IPV6:
		break;
	case LI_PLUGIN_CORE_LISTEN_MASK_UNIX:
		g_string_free(mask->value.unix_socket.path, TRUE);
		break;
	}
	g_slice_free(liPluginCoreListenMask, mask);
}

static void core_listen_parse(liServer *srv, liPlugin *p, liValue **options) {
	liPluginCoreConfig *config = (liPluginCoreConfig*) p->data;
	gboolean have_type = FALSE;

	liPluginCoreListenMask *mask = g_slice_new0(liPluginCoreListenMask);

	if (options[0]) { /* ip */
		if (have_type) goto only_one_type;
		have_type = TRUE;
		if (li_parse_ipv4(options[0]->data.string->str, &mask->value.ipv4.addr, &mask->value.ipv4.networkmask, &mask->value.ipv4.port)) {
			mask->type = LI_PLUGIN_CORE_LISTEN_MASK_IPV4;
		} else if (li_parse_ipv6(options[0]->data.string->str, mask->value.ipv6.addr, &mask->value.ipv6.network, &mask->value.ipv6.port)) {
			mask->type = LI_PLUGIN_CORE_LISTEN_MASK_IPV6;
		} else {
			ERROR(srv, "couldn't parse ip/network:port in listen mask '%s'", options[0]->data.string->str);
			config->load_failed = FALSE;
			g_slice_free(liPluginCoreListenMask, mask);
			return;
		}
	}

	if (options[1]) { /* unix */
		if (have_type) goto only_one_type;
		have_type = TRUE;
		mask->type = LI_PLUGIN_CORE_LISTEN_MASK_UNIX;
		mask->value.unix_socket.path = g_string_new_len(GSTR_LEN(options[2]->data.string));
	}

	if (!have_type) {
		ERROR(srv, "%s", "no options found in listen mask");
		config->load_failed = FALSE;
		g_slice_free(liPluginCoreListenMask, mask);
		return;
	}

	g_ptr_array_add(config->load_listen_masks, mask);
	return;

only_one_type:
	ERROR(srv, "%s", "you can only use one of 'ip' and 'unix' in listen masks");
	config->load_failed = FALSE;
	g_slice_free(liPluginCoreListenMask, mask);
	return;
}

static const liPluginItemOption core_listen_options[] = {
	/*  0 */ { "ip", LI_VALUE_STRING, 0 },
	/*  1 */ { "unix", LI_VALUE_STRING, 0 },
	{ NULL, 0, 0 }
};


static const liPluginItem core_items[] = {
	{ "instance", core_instance_parse, core_instance_options },
	{ "listen", core_listen_parse, core_listen_options },
	{ NULL, NULL, NULL }
};

static int do_listen(liServer *srv, liPluginCoreConfig *config, GString *str) {
	guint32 ipv4;
#ifdef HAVE_IPV6
	guint8 ipv6[16];
#endif
	guint16 port;
	guint i;
	liPluginCoreListenMask *mask;

	if (li_parse_ipv4(str->str, &ipv4, NULL, &port)) {
		int s, v;
		struct sockaddr_in addr;
		memset(&addr, 0, sizeof(addr));

		if (!port) port = 80;

		if (config->listen_masks->len) {
			for (i = 0; i < config->listen_masks->len; i++) {
				mask = g_ptr_array_index(config->listen_masks, i);
				switch (mask->type) {
				case LI_PLUGIN_CORE_LISTEN_MASK_IPV4:
					if (!li_ipv4_in_ipv4_net(ipv4, mask->value.ipv4.addr, mask->value.ipv4.networkmask)) continue;
					if ((mask->value.ipv4.port != port) && (mask->value.ipv4.port != 0 || (port != 80 && port != 443))) continue;
					break;
				case LI_PLUGIN_CORE_LISTEN_MASK_IPV6:
					if (!li_ipv4_in_ipv6_net(ipv4, mask->value.ipv6.addr, mask->value.ipv6.network)) continue;
					if ((mask->value.ipv6.port != port) && (mask->value.ipv6.port != 0 || (port != 80 && port != 443))) continue;
					break;
				case LI_PLUGIN_CORE_LISTEN_MASK_UNIX:
					continue;
				}
				break;
			}
			if (i == config->listen_masks->len) {
				ERROR(srv, "listen to socket '%s' not allowed", str->str);
				return -1;
			}
		}

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
	} else if (li_parse_ipv6(str->str, ipv6, NULL, &port)) {
		GString *ipv6_str = g_string_sized_new(0);
		int s, v;
		struct sockaddr_in6 addr;
		li_ipv6_tostring(ipv6_str, ipv6);
		if (!port) port = 80;

		if (config->listen_masks->len) {
			for (i = 0; i < config->listen_masks->len; i++) {
				mask = g_ptr_array_index(config->listen_masks, i);
				switch (mask->type) {
				case LI_PLUGIN_CORE_LISTEN_MASK_IPV4:
					if (!li_ipv6_in_ipv4_net(ipv6, mask->value.ipv4.addr, mask->value.ipv4.networkmask)) continue;
					if ((mask->value.ipv4.port != port) && (mask->value.ipv4.port != 0 || (port != 80 && port != 443))) continue;
					break;
				case LI_PLUGIN_CORE_LISTEN_MASK_IPV6:
					if (!li_ipv6_in_ipv6_net(ipv6, mask->value.ipv6.addr, mask->value.ipv6.network)) continue;
					if ((mask->value.ipv6.port != port) && (mask->value.ipv6.port != 0 || (port != 80 && port != 443))) continue;
					break;
				case LI_PLUGIN_CORE_LISTEN_MASK_UNIX:
					continue;
				}
				break;
			}
			if (i == config->listen_masks->len) {
				ERROR(srv, "listen to socket '%s' not allowed", str->str);
				return -1;
			}
		}

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
/* TODO: listen unix socket */
	} else {
		ERROR(srv, "Invalid ip: '%s'", str->str);
		return -1;
	}
}

static void core_listen(liServer *srv, liInstance *i, liPlugin *p, gint32 id, GString *data) {
	GError *err = NULL;
	gint fd;
	GArray *fds;
	liPluginCoreConfig *config = (liPluginCoreConfig*) p->data;

	DEBUG(srv, "core_listen(%i) '%s'", id, data->str);

	if (-1 == id) return; /* ignore simple calls */

	fd = do_listen(srv, config, data);

	if (-1 == fd) {
		GString *error = g_string_sized_new(0);
		g_string_printf(error, "Couldn't listen to '%s'", data->str);
		if (!li_angel_send_result(i->acon, id, error, NULL, NULL, &err)) {
			ERROR(srv, "Couldn't send result: %s", err->message);
			g_error_free(err);
		}
		return;
	}

	fds = g_array_new(FALSE, FALSE, sizeof(int));
	g_array_append_val(fds, fd);

	if (!li_angel_send_result(i->acon, id, NULL, NULL, fds, &err)) {
		ERROR(srv, "Couldn't send result: %s", err->message);
		g_error_free(err);
		return;
	}
}

static void core_reached_state(liServer *srv, liInstance *i, liPlugin *p, gint32 id, GString *data) {
	UNUSED(srv);
	UNUSED(p);
	UNUSED(id);

	if (0 == strcmp(data->str, "suspended")) {
		li_instance_state_reached(i, LI_INSTANCE_SUSPENDED);
	} else if (0 == strcmp(data->str, "warmup")) {
		li_instance_state_reached(i, LI_INSTANCE_WARMUP);
	} else if (0 == strcmp(data->str, "running")) {
		li_instance_state_reached(i, LI_INSTANCE_RUNNING);
	} else if (0 == strcmp(data->str, "suspending")) {
		li_instance_state_reached(i, LI_INSTANCE_SUSPENDING);
	}
}

static void core_clean(liServer *srv, liPlugin *p);
static void core_free(liServer *srv, liPlugin *p) {
	liPluginCoreConfig *config = (liPluginCoreConfig*) p->data;
	guint i;

	core_clean(srv, p);

	if (config->instconf) {
		li_instance_conf_release(config->instconf);
		config->instconf = NULL;
	}

	if (config->inst) {
		li_instance_set_state(config->inst, LI_INSTANCE_DOWN);
		li_instance_release(config->inst);
		config->inst = NULL;
	}

	for (i = 0; i < config->listen_masks->len; i++) {
		core_listen_mask_free(g_ptr_array_index(config->listen_masks, i));
	}
	g_ptr_array_free(config->listen_masks, TRUE);
	g_ptr_array_free(config->load_listen_masks, TRUE);
	config->listen_masks = NULL;
	config->load_listen_masks = NULL;
}

static void core_clean(liServer *srv, liPlugin *p) {
	liPluginCoreConfig *config = (liPluginCoreConfig*) p->data;
	guint i;
	UNUSED(srv);

	if (config->load_instconf) {
		li_instance_conf_release(config->load_instconf);
		config->load_instconf = NULL;
	}

	for (i = 0; i < config->load_listen_masks->len; i++) {
		core_listen_mask_free(g_ptr_array_index(config->load_listen_masks, i));
	}
	g_ptr_array_set_size(config->load_listen_masks, 0);

	config->load_failed = FALSE;
}

static gboolean core_check(liServer *srv, liPlugin *p) {
	liPluginCoreConfig *config = (liPluginCoreConfig*) p->data;
	UNUSED(srv);
	return !config->load_failed;
}

static void core_activate(liServer *srv, liPlugin *p) {
	liPluginCoreConfig *config = (liPluginCoreConfig*) p->data;
	GPtrArray *tmp_ptrarray;
	guint i;

	if (config->instconf) {
		li_instance_conf_release(config->instconf);
		config->instconf = NULL;
	}

	if (config->inst) {
		li_instance_set_state(config->inst, LI_INSTANCE_FINISHED);
		li_instance_release(config->inst);
		config->inst = NULL;
	}

	for (i = 0; i < config->listen_masks->len; i++) {
		core_listen_mask_free(g_ptr_array_index(config->listen_masks, i));
	}
	g_ptr_array_set_size(config->listen_masks, 0);


	config->instconf = config->load_instconf;
	config->load_instconf = NULL;

	tmp_ptrarray = config->load_listen_masks; config->load_listen_masks = config->listen_masks; config->listen_masks = tmp_ptrarray;

	if (config->instconf) {
		config->inst = li_server_new_instance(srv, config->instconf);
		li_instance_set_state(config->inst, LI_INSTANCE_RUNNING);
	}
}

static gboolean core_init(liServer *srv, liPlugin *p) {
	liPluginCoreConfig *config;
	UNUSED(srv);
	p->data = config = g_slice_new0(liPluginCoreConfig);
	p->items = core_items;

	p->handle_free = core_free;
	p->handle_clean_config = core_clean;
	p->handle_check_config = core_check;
	p->handle_activate_config = core_activate;

	config->listen_masks = g_ptr_array_new();
	config->load_listen_masks = g_ptr_array_new();

	g_hash_table_insert(p->angel_callbacks, "listen", (gpointer)(intptr_t)core_listen);
	g_hash_table_insert(p->angel_callbacks, "reached-state", (gpointer)(intptr_t)core_reached_state);

	return TRUE;
}

gboolean li_plugin_core_init(liServer *srv) {
	/* load core plugins */
	return NULL != li_angel_plugin_register(srv, NULL, "core", core_init);
}

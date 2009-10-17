
#include <lighttpd/angel_plugin_core.h>
#include <lighttpd/ip_parsers.h>

typedef struct listen_socket listen_socket;
typedef struct listen_ref_resource listen_ref_resource;

struct listen_socket {
	gint refcount;

	liSocketAddress addr;
	int fd;
};

struct listen_ref_resource {
	liInstanceResource ires;

	listen_socket *sock;
};

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

static listen_socket* listen_new_socket(liSocketAddress *addr, int fd) {
	listen_socket *sock = g_slice_new0(listen_socket);

	sock->refcount = 0;

	sock->addr = *addr;
	sock->fd = fd;

	return sock;
}

static void listen_socket_acquire(listen_socket *sock) {
	g_atomic_int_inc(&sock->refcount);
}

static void listen_ref_release(liServer *srv, liInstance *i, liPlugin *p, liInstanceResource *res) {
	listen_ref_resource *ref = res->data;
	listen_socket *sock = ref->sock;
	UNUSED(i);
	UNUSED(srv);

	assert(g_atomic_int_get(&sock->refcount) > 0);
	if (g_atomic_int_dec_and_test(&sock->refcount)) {
		liPluginCoreConfig *config = (liPluginCoreConfig*) p->data;

		g_hash_table_remove(config->listen_sockets, &sock->addr);
	}

	g_slice_free(listen_ref_resource, ref);
}

static void _listen_socket_free(gpointer ptr) {
	listen_socket *sock = ptr;

	li_sockaddr_clear(&sock->addr);
	close(sock->fd);

	g_slice_free(listen_socket, sock);
}

static void listen_socket_add(liInstance *i, liPlugin *p, listen_socket *sock) {
	listen_ref_resource *ref = g_slice_new0(listen_ref_resource);

	listen_socket_acquire(sock);
	ref->sock = sock;

	li_instance_add_resource(i, &ref->ires, listen_ref_release, p, ref);
}

static gboolean listen_check_acl(liServer *srv, liPluginCoreConfig *config, liSocketAddress *addr) {
	guint i;
	liPluginCoreListenMask *mask;

	switch (addr->addr->plain.sa_family) {
	case AF_INET: {
		struct sockaddr_in *ipv4 = &addr->addr->ipv4;
		guint port = ntohs(ipv4->sin_port);

		if (config->listen_masks->len) {
			for (i = 0; i < config->listen_masks->len; i++) {
				mask = g_ptr_array_index(config->listen_masks, i);
				switch (mask->type) {
				case LI_PLUGIN_CORE_LISTEN_MASK_IPV4:
					if (!li_ipv4_in_ipv4_net(ipv4->sin_addr.s_addr, mask->value.ipv4.addr, mask->value.ipv4.networkmask)) continue;
					if ((mask->value.ipv4.port != port) && (mask->value.ipv4.port != 0 || (port != 80 && port != 443))) continue;
					return TRUE;
				case LI_PLUGIN_CORE_LISTEN_MASK_IPV6:
					if (!li_ipv4_in_ipv6_net(ipv4->sin_addr.s_addr, mask->value.ipv6.addr, mask->value.ipv6.network)) continue;
					if ((mask->value.ipv6.port != port) && (mask->value.ipv6.port != 0 || (port != 80 && port != 443))) continue;
					return TRUE;
				default:
					continue;
				}
			}
			return FALSE;
		} else {
			return (ipv4->sin_port == 80 || ipv4->sin_port == 443);
		}
	} break;
#ifdef HAVE_IPV6
	case AF_INET6: {
		struct sockaddr_in6 *ipv6 = &addr->addr->ipv6;
		guint port = ntohs(ipv6->sin6_port);

		if (config->listen_masks->len) {
			for (i = 0; i < config->listen_masks->len; i++) {
				mask = g_ptr_array_index(config->listen_masks, i);
				switch (mask->type) {
				case LI_PLUGIN_CORE_LISTEN_MASK_IPV4:
					if (!li_ipv6_in_ipv4_net(ipv6->sin6_addr.s6_addr, mask->value.ipv4.addr, mask->value.ipv4.networkmask)) continue;
					if ((mask->value.ipv4.port != port) && (mask->value.ipv4.port != 0 || (port != 80 && port != 443))) continue;
					return TRUE;
				case LI_PLUGIN_CORE_LISTEN_MASK_IPV6:
					if (!li_ipv6_in_ipv6_net(ipv6->sin6_addr.s6_addr, mask->value.ipv6.addr, mask->value.ipv6.network)) continue;
					if ((mask->value.ipv6.port != port) && (mask->value.ipv6.port != 0 || (port != 80 && port != 443))) continue;
					return TRUE;
				default:
					continue;
				}
			}
			return FALSE;
		} else {
			return (ipv6->sin6_port == 80 || ipv6->sin6_port == 443);
		}
	} break;
#endif
#ifdef HAVE_SYS_UN_H
	case AF_UNIX: {
		if (config->listen_masks->len) {
			/* TODO: support unix addresses */
		} else {
			return FALSE; /* don't allow unix by default */
		}
	} break;
#endif
	default:
		ERROR(srv, "Address family %i not supported", addr->addr->plain.sa_family);
		break;
	}
	return FALSE;
}

static int do_listen(liServer *srv, liSocketAddress *addr, GString *str) {
	int s, v;
	GString *ipv6_str;

	switch (addr->addr->plain.sa_family) {
	case AF_INET:
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
		if (-1 == bind(s, &addr->addr->plain, addr->len)) {
			close(s);
			ERROR(srv, "Couldn't bind socket to '%s': %s", str->str, g_strerror(errno));
			return -1;
		}
		if (-1 == listen(s, 1000)) {
			close(s);
			ERROR(srv, "Couldn't listen on '%s': %s", str->str, g_strerror(errno));
			return -1;
		}
		DEBUG(srv, "listen to ipv4: '%s' port: %d", str->str, addr->addr->ipv4.sin_port);
		return s;
#ifdef HAVE_IPV6
	case AF_INET6:
		ipv6_str = g_string_sized_new(0);
		li_ipv6_tostring(ipv6_str, addr->addr->ipv6.sin6_addr.s6_addr);

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
		if (-1 == bind(s, &addr->addr->plain, addr->len)) {
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
		DEBUG(srv, "listen to ipv6: '%s' port: %d", ipv6_str->str, addr->addr->ipv6.sin6_port);
		g_string_free(ipv6_str, TRUE);
		return s;
#endif
#ifdef HAVE_SYS_UN_H
	case AF_UNIX:
		ERROR(srv, "Unix sockets not supported: %s", str->str);
		/* TODO: support unix addresses */
		break;
#endif
	default:
		ERROR(srv, "Address family %i not supported", addr->addr->plain.sa_family);
		break;
	}
	return -1;
}

static void core_listen(liServer *srv, liPlugin *p, liInstance *i, gint32 id, GString *data) {
	GError *err = NULL;
	gint fd;
	GArray *fds;
	liPluginCoreConfig *config = (liPluginCoreConfig*) p->data;
	liSocketAddress addr;
	listen_socket *sock;

	DEBUG(srv, "core_listen(%i) '%s'", id, data->str);

	if (-1 == id) return; /* ignore simple calls */

	addr = li_sockaddr_from_string(data, 80);
	if (!addr.addr) {
		GString *error = g_string_sized_new(0);
		g_string_printf(error, "Invalid socket address: '%s'", data->str);
		if (!li_angel_send_result(i->acon, id, error, NULL, NULL, &err)) {
			ERROR(srv, "Couldn't send result: %s", err->message);
			g_error_free(err);
		}
		return;
	}

	if (!listen_check_acl(srv, config, &addr)) {
		GString *error = g_string_sized_new(0);
		li_sockaddr_clear(&addr);
		g_string_printf(error, "Socket address not allowed: '%s'", data->str);
		if (!li_angel_send_result(i->acon, id, error, NULL, NULL, &err)) {
			ERROR(srv, "Couldn't send result: %s", err->message);
			g_error_free(err);
		}
		return;
	}

	if (NULL == (sock = g_hash_table_lookup(config->listen_sockets, &addr))) {
		fd = do_listen(srv, &addr, data);

		if (-1 == fd) {
			GString *error = g_string_sized_new(0);
			li_sockaddr_clear(&addr);
			g_string_printf(error, "Couldn't listen to '%s'", data->str);
			if (!li_angel_send_result(i->acon, id, error, NULL, NULL, &err)) {
				ERROR(srv, "Couldn't send result: %s", err->message);
				g_error_free(err);
			}
			return;
		}

		li_fd_init(fd);
		sock = listen_new_socket(&addr, fd);
		g_hash_table_insert(config->listen_sockets, &sock->addr, sock);
	} else {
		li_sockaddr_clear(&addr);
	}

	listen_socket_add(i, p, sock);

	fd = dup(sock->fd);

	if (-1 == fd) {
		/* socket ref will be released when instance is released */
		GString *error = g_string_sized_new(0);
		g_string_printf(error, "Couldn't duplicate fd");
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

static void core_reached_state(liServer *srv, liPlugin *p, liInstance *i, gint32 id, GString *data) {
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

	li_ev_safe_ref_and_stop(ev_signal_stop, srv->loop, &config->sig_hup);

	core_clean(srv, p);

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
	g_ptr_array_free(config->listen_masks, TRUE);
	g_ptr_array_free(config->load_listen_masks, TRUE);
	g_hash_table_destroy(config->listen_sockets);
	config->listen_masks = NULL;
	config->load_listen_masks = NULL;

	g_slice_free(liPluginCoreConfig, config);
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

static void core_instance_replaced(liServer *srv, liPlugin *p, liInstance *oldi, liInstance *newi) {
	liPluginCoreConfig *config = (liPluginCoreConfig*) p->data;
	UNUSED(srv);

	if (oldi == config->inst && LI_INSTANCE_FINISHED == oldi->s_cur) {
		li_instance_acquire(newi);
		config->inst = newi;
		li_instance_release(oldi);
	}
}

static void core_handle_sig_hup(struct ev_loop *loop, ev_signal *w, int revents) {
	liPluginCoreConfig *config = w->data;
	liInstance *oldi, *newi;
	UNUSED(loop);
	UNUSED(revents);

	if (NULL == (oldi = config->inst)) return;

	if (oldi->replace_by) return;
	newi = li_server_new_instance(oldi->srv, config->instconf);
	li_instance_replace(oldi, newi);
	li_instance_release(newi);
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
	p->handle_instance_replaced = core_instance_replaced;

	config->listen_masks = g_ptr_array_new();
	config->load_listen_masks = g_ptr_array_new();
	config->listen_sockets = g_hash_table_new_full(li_hash_sockaddr, li_equal_sockaddr, NULL, _listen_socket_free);

	li_angel_plugin_add_angel_cb(p, "listen", core_listen);
	li_angel_plugin_add_angel_cb(p, "reached-state", core_reached_state);

	ev_signal_init(&config->sig_hup, core_handle_sig_hup, SIGHUP);
	config->sig_hup.data = config;
	ev_signal_start(srv->loop, &config->sig_hup);
	ev_unref(srv->loop);

	return TRUE;
}

gboolean li_plugin_core_init(liServer *srv) {
	/* load core plugins */
	return NULL != li_angel_plugin_register(srv, NULL, "core", core_init);
}


#include <lighttpd/angel_plugin_core.h>
#include <lighttpd/angel_config_parser.h>
#include <lighttpd/ip_parsers.h>

#include <fnmatch.h>
#include <fcntl.h>

#include <pwd.h>
#include <grp.h>

typedef struct listen_socket listen_socket;
typedef struct listen_ref_resource listen_ref_resource;

#ifndef DEFAULT_LIBEXECDIR
# define DEFAULT_LIBEXECDIR "/usr/local/lib/lighttpd2"
#endif

struct listen_socket {
	gint refcount;

	liSocketAddress addr;
	int fd;
};

struct listen_ref_resource {
	liInstanceResource ires;

	listen_socket *sock;
};

static liValue* core_parse_check_parameter_string(liValue *value, const char* item, GError **err) {
	value = li_value_get_single_argument(value);
	if (LI_VALUE_STRING != li_value_type(value)) {
		g_set_error(err, LI_ANGEL_CONFIG_PARSER_ERROR, LI_ANGEL_CONFIG_PARSER_ERROR_PARSE,
			"%s: expecting a string as parameter", item);
		return NULL;
	}

	return value;
}
/* destroys value, extracting the contained string into *target */
static gboolean core_parse_store_string(liValue *value, const char* item, GString** target, GError **err) {
	if (NULL != *target) {
		g_set_error(err, LI_ANGEL_CONFIG_PARSER_ERROR, LI_ANGEL_CONFIG_PARSER_ERROR_PARSE,
			"%s: already specified, can only be used once", item);
		return FALSE;
	}

	if (NULL == (value = core_parse_check_parameter_string(value, item, err))) return FALSE;
	*target = li_value_extract_string(value);

	return TRUE;
}
/* destroys value, adding the contained strings (char*) to target */
static gboolean core_parse_store_string_list(liValue *value, const char* item, GPtrArray* list, GError **err) {
	value = li_value_get_single_argument(value);
	if (LI_VALUE_STRING == li_value_type(value)) {
		li_value_wrap_in_list(value);
	}
	else if (LI_VALUE_LIST != li_value_type(value)) goto parameter_type_error;

	LI_VALUE_FOREACH(entry, value)
		if (LI_VALUE_STRING != li_value_type(entry)) goto parameter_type_error;

		g_ptr_array_add(list, g_string_free(li_value_extract_string(entry), FALSE));
	LI_VALUE_END_FOREACH()

	return TRUE;

parameter_type_error:
	g_set_error(err, LI_ANGEL_CONFIG_PARSER_ERROR, LI_ANGEL_CONFIG_PARSER_ERROR_PARSE,
		"%s: expecting string list as parameter", item);
	return FALSE;
}
/* extract the contained integer into *target */
static gboolean core_parse_store_integer(liValue *value, const char* item, gint64* target, GError **err) {
	value = li_value_get_single_argument(value);
	if (LI_VALUE_NUMBER != li_value_type(value)) {
		g_set_error(err, LI_ANGEL_CONFIG_PARSER_ERROR, LI_ANGEL_CONFIG_PARSER_ERROR_PARSE,
			"%s: expecting a number as parameter", item);
		return FALSE;
	}

	*target = value->data.number;

	return TRUE;
}


static gboolean core_parse_user(liServer *srv, liPlugin *p, liValue *value, GError **err) {
	liPluginCoreConfig *pc = p->data;
	struct passwd *pwd;
	GString *user;
	UNUSED(srv);

	if (!core_parse_store_string(value, "user", &pc->parsing.user, err)) return FALSE;

	user = pc->parsing.user;
	if (NULL == (pwd = getpwnam(user->str))) {
		g_set_error(err, LI_ANGEL_CONFIG_PARSER_ERROR, LI_ANGEL_CONFIG_PARSER_ERROR_PARSE,
			"user: couldn't find user '%s' ", user->str);
		return FALSE;
	}
	if (0 == pwd->pw_uid) {
		g_set_error(err, LI_ANGEL_CONFIG_PARSER_ERROR, LI_ANGEL_CONFIG_PARSER_ERROR_PARSE,
			"user: will not changed to uid 0");
		return FALSE;
	}
	if (0 == pwd->pw_gid) {
		g_set_error(err, LI_ANGEL_CONFIG_PARSER_ERROR, LI_ANGEL_CONFIG_PARSER_ERROR_PARSE,
			"user: will not changed to gid 0");
		return FALSE;
	}

	pc->parsing.user_uid = pwd->pw_uid;
	pc->parsing.user_gid = pwd->pw_gid;

	return TRUE;
}

static gboolean core_parse_group(liServer *srv, liPlugin *p, liValue *value, GError **err) {
	liPluginCoreConfig *pc = p->data;
	struct group *grp;
	GString *group;
	UNUSED(srv);

	if (!core_parse_store_string(value, "group", &pc->parsing.group, err)) return FALSE;

	group = pc->parsing.group;
	if (NULL == (grp = getgrnam(group->str))) {
		g_set_error(err, LI_ANGEL_CONFIG_PARSER_ERROR, LI_ANGEL_CONFIG_PARSER_ERROR_PARSE,
			"group: couldn't find group '%s' ", group->str);
		return FALSE;
	}
	if (0 == grp->gr_gid) {
		g_set_error(err, LI_ANGEL_CONFIG_PARSER_ERROR, LI_ANGEL_CONFIG_PARSER_ERROR_PARSE,
			"group: will not changed to gid 0");
		return FALSE;
	}

	pc->parsing.group_gid = grp->gr_gid;

	return TRUE;
}

static gboolean core_parse_binary(liServer *srv, liPlugin *p, liValue *value, GError **err) {
	liPluginCoreConfig *pc = p->data;
	UNUSED(srv);

	return core_parse_store_string(value, "binary", &pc->parsing.binary, err);
}

static gboolean core_parse_config(liServer *srv, liPlugin *p, liValue *value, GError **err) {
	liPluginCoreConfig *pc = p->data;
	UNUSED(srv);

	if (NULL != pc->parsing.luaconfig) {
		g_set_error(err, LI_ANGEL_CONFIG_PARSER_ERROR, LI_ANGEL_CONFIG_PARSER_ERROR_PARSE,
			"config: already specified luaconfig");
		return FALSE;
	}

	return core_parse_store_string(value, "config", &pc->parsing.config, err);
}

static gboolean core_parse_luaconfig(liServer *srv, liPlugin *p, liValue *value, GError **err) {
	liPluginCoreConfig *pc = p->data;
	UNUSED(srv);

	if (NULL != pc->parsing.config) {
		g_set_error(err, LI_ANGEL_CONFIG_PARSER_ERROR, LI_ANGEL_CONFIG_PARSER_ERROR_PARSE,
			"luaconfig: already specified config");
		return FALSE;
	}

	return core_parse_store_string(value, "luaconfig", &pc->parsing.luaconfig, err);
}

static gboolean core_parse_modules_path(liServer *srv, liPlugin *p, liValue *value, GError **err) {
	liPluginCoreConfig *pc = p->data;
	UNUSED(srv);

	return core_parse_store_string(value, "modules_path", &pc->parsing.modules_path, err);
}

static void add_env(GPtrArray *env, const char *key, size_t keylen, const char *value, size_t valuelen) {
	gchar* entry = g_malloc(keylen + 1 /* "=" */ + valuelen + 1 /* \0 */);
	memcpy(entry, key, keylen);
	entry[keylen] = '=';
	memcpy(entry + keylen + 1, value, valuelen);
	entry[keylen + valuelen + 1] = '\0';
	g_ptr_array_add(env, entry);
}

static gboolean core_parse_env(liServer *srv, liPlugin *p, liValue *value, GError **err) {
	liPluginCoreConfig *pc = p->data;
	UNUSED(srv);

	value = li_value_get_single_argument(value);
	if (LI_VALUE_LIST != li_value_type(value)) goto parameter_type_error;
	if (li_value_list_has_len(value, 2) && LI_VALUE_STRING == li_value_list_type_at(value, 0) && LI_VALUE_STRING == li_value_list_type_at(value, 1)) {
		if (NULL == strchr(li_value_list_at(value, 0)->data.string->str, '=')) {
			/* no '=' in first string: single key => value pair; otherwise a list with two entries ['foo=x', 'bar=y'] */
			li_value_wrap_in_list(value);
		}
	}

	LI_VALUE_FOREACH(entry, value)
		if (LI_VALUE_STRING == li_value_type(entry)) {
			g_ptr_array_add(pc->parsing.env, g_string_free(li_value_extract_string(entry), FALSE));
		} else {
			liValue *key = li_value_list_at(entry, 0);
			liValue *val = li_value_list_at(entry, 1);
			if (!li_value_list_has_len(entry, 2) || LI_VALUE_STRING != li_value_type(key) || LI_VALUE_STRING != li_value_type(val)) goto parameter_type_error;
			add_env(pc->parsing.env, GSTR_LEN(key->data.string), GSTR_LEN(val->data.string));
		}
	LI_VALUE_END_FOREACH()

	return TRUE;

parameter_type_error:
	g_set_error(err, LI_ANGEL_CONFIG_PARSER_ERROR, LI_ANGEL_CONFIG_PARSER_ERROR_PARSE,
		"env: expecting key-value/string list as parameter");
	return FALSE;
}

static gboolean core_parse_copy_env(liServer *srv, liPlugin *p, liValue *value, GError **err) {
	liPluginCoreConfig *pc = p->data;
	UNUSED(srv);

	value = li_value_get_single_argument(value);
	if (LI_VALUE_STRING == li_value_type(value)) {
		li_value_wrap_in_list(value);
	}
	else if (LI_VALUE_LIST != li_value_type(value)) goto parameter_type_error;

	LI_VALUE_FOREACH(entry, value)
		const char *val;
		size_t vallen;
		if (LI_VALUE_STRING != li_value_type(entry)) goto parameter_type_error;

		val = getenv(entry->data.string->str);
		if (NULL == val) continue;
		vallen = strlen(val);

		add_env(pc->parsing.env, GSTR_LEN(entry->data.string), val, vallen);
	LI_VALUE_END_FOREACH()

	return TRUE;

parameter_type_error:
	g_set_error(err, LI_ANGEL_CONFIG_PARSER_ERROR, LI_ANGEL_CONFIG_PARSER_ERROR_PARSE,
		"copy_env: expecting string list as parameter");
	return FALSE;
}

static gboolean core_parse_wrapper(liServer *srv, liPlugin *p, liValue *value, GError **err) {
	liPluginCoreConfig *pc = p->data;
	UNUSED(srv);

	return core_parse_store_string_list(value, "wrapper", pc->parsing.wrapper, err);
}

static gboolean core_parse_max_core_file_size(liServer *srv, liPlugin *p, liValue *value, GError **err) {
	liPluginCoreConfig *pc = p->data;
	UNUSED(srv);

	if (-1 != pc->parsing.rlim_core) {
		g_set_error(err, LI_ANGEL_CONFIG_PARSER_ERROR, LI_ANGEL_CONFIG_PARSER_ERROR_PARSE,
			"max_core_file_size: already specified");
		return FALSE;
	}

	return core_parse_store_integer(value, "max_core_file_size", &pc->parsing.rlim_core, err);
}

static gboolean core_parse_max_open_files(liServer *srv, liPlugin *p, liValue *value, GError **err) {
	liPluginCoreConfig *pc = p->data;
	UNUSED(srv);

	if (-1 != pc->parsing.rlim_nofile) {
		g_set_error(err, LI_ANGEL_CONFIG_PARSER_ERROR, LI_ANGEL_CONFIG_PARSER_ERROR_PARSE,
			"max_open_files: already specified");
		return FALSE;
	}

	return core_parse_store_integer(value, "max_open_files", &pc->parsing.rlim_nofile, err);
}




static void core_listen_mask_free(liPluginCoreListenMask *mask) {
	if (NULL == mask) return;

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

static gboolean core_parse_allow_listen(liServer *srv, liPlugin *p, liValue *value, GError **err) {
	liPluginCoreConfig *pc = p->data;
	UNUSED(srv);

	value = li_value_get_single_argument(value);
	if (LI_VALUE_LIST != li_value_type(value)) {
		li_value_wrap_in_list(value);
	}

	LI_VALUE_FOREACH(entry, value)
		GString *s;
		liPluginCoreListenMask *mask;

		if (LI_VALUE_STRING != li_value_type(entry)) goto parameter_type_error;
		s = entry->data.string;

		mask = g_slice_new0(liPluginCoreListenMask);
		if (li_string_prefix(s, CONST_STR_LEN("unix:/"))) {
			mask->type = LI_PLUGIN_CORE_LISTEN_MASK_UNIX;
			mask->value.unix_socket.path = li_value_extract_string(entry);
			g_string_erase(mask->value.unix_socket.path, 0, 5); /* remove "unix:" prefix */
		}
		else if (li_parse_ipv4(s->str, &mask->value.ipv4.addr, &mask->value.ipv4.networkmask, &mask->value.ipv4.port)) {
			mask->type = LI_PLUGIN_CORE_LISTEN_MASK_IPV4;
		}
		else if (li_parse_ipv6(s->str, mask->value.ipv6.addr, &mask->value.ipv6.network, &mask->value.ipv6.port)) {
			mask->type = LI_PLUGIN_CORE_LISTEN_MASK_IPV6;
		}
		else {
			g_set_error(err, LI_ANGEL_CONFIG_PARSER_ERROR, LI_ANGEL_CONFIG_PARSER_ERROR_PARSE,
				"allow_listen: couldn't parse socket address mask '%s'", s->str);
			g_slice_free(liPluginCoreListenMask, mask);
			return FALSE;
		}

		g_ptr_array_add(pc->parsing.listen_masks, mask);
	LI_VALUE_END_FOREACH()

	return TRUE;

parameter_type_error:
	g_set_error(err, LI_ANGEL_CONFIG_PARSER_ERROR, LI_ANGEL_CONFIG_PARSER_ERROR_PARSE,
		"allow_listen: expecting string list as parameter");
	return FALSE;
}


static const liPluginItem core_items[] = {
	{ "user", core_parse_user },
	{ "group", core_parse_group },
	{ "binary", core_parse_binary },
	{ "config", core_parse_config },
	{ "luaconfig", core_parse_luaconfig },
	{ "modules_path", core_parse_modules_path },
	{ "wrapper", core_parse_wrapper },
	{ "env", core_parse_env },
	{ "copy_env", core_parse_copy_env },
	{ "max_core_file_size", core_parse_max_core_file_size },
	{ "max_open_files", core_parse_max_open_files },
	{ "allow_listen", core_parse_allow_listen },
	{ NULL, NULL }
};

#define INIT_STR(N) /* GString, init to NULL */ \
	if (NULL != pc->parsing.N) { \
		g_string_free(pc->parsing.N, TRUE); \
		pc->parsing.N = NULL; \
	}
#define INIT_STR_LIST(N) /* char* ptr array, init to empty array */ \
	if (NULL != pc->parsing.N) { \
		GPtrArray *a_ ## N = pc->parsing.N; \
		guint i_ ## N; \
		for (i_ ## N = 0; i_ ## N < a_ ## N->len; ++i_ ## N) { \
			g_free(g_ptr_array_index(a_ ## N, i_ ## N)); \
		} \
		g_ptr_array_set_size(a_ ## N, 0); \
	} else { \
		pc->parsing.N = g_ptr_array_new(); \
	}

static void core_parse_init(liServer *srv, liPlugin *p) {
	liPluginCoreConfig *pc = p->data;
	UNUSED(srv);

	INIT_STR_LIST(env);
	INIT_STR(user);
	pc->parsing.user_uid = (uid_t) -1;
	pc->parsing.user_gid = (gid_t) -1;

	INIT_STR(group);
	pc->parsing.group_gid = (gid_t) -1;

	INIT_STR(binary);
	INIT_STR(config);
	INIT_STR(luaconfig);
	INIT_STR(modules_path);

	INIT_STR_LIST(wrapper);

	pc->parsing.rlim_core = pc->parsing.rlim_nofile = -1;

	if (NULL != pc->parsing.instconf) {
		li_instance_conf_release(pc->parsing.instconf);
		pc->parsing.instconf = NULL;
	}

	if (NULL != pc->parsing.listen_masks) {
		GPtrArray *a = pc->parsing.listen_masks;
		guint i;
		for (i = 0; i < a->len; ++i) {
			core_listen_mask_free(g_ptr_array_index(a, i));
		}
		g_ptr_array_set_size(a, 0);
	} else {
		pc->parsing.listen_masks = g_ptr_array_new();
	}
}

static gboolean core_check(liServer *srv, liPlugin *p, GError **err) {
	GPtrArray *cmd;
	GString *user;
	gchar **cmdarr, **envarr;
	liPluginCoreConfig *pc = p->data;
	gid_t gid = ((gid_t)-1 != pc->parsing.group_gid) ? pc->parsing.group_gid : pc->parsing.user_gid;
	UNUSED(srv);
	UNUSED(err);

	cmd = pc->parsing.wrapper;
	pc->parsing.wrapper = g_ptr_array_new();

	if (NULL != pc->parsing.binary) {
		g_ptr_array_add(cmd, g_string_free(pc->parsing.binary, FALSE));
		pc->parsing.binary = NULL;
	} else {
		g_ptr_array_add(cmd, g_strndup(CONST_STR_LEN(DEFAULT_LIBEXECDIR "/lighttpd2-worker")));
	}

	g_ptr_array_add(cmd, g_strndup(CONST_STR_LEN("--angel")));
	g_ptr_array_add(cmd, g_strndup(CONST_STR_LEN("-c")));
	if (NULL != pc->parsing.config) {
		g_ptr_array_add(cmd, g_string_free(pc->parsing.config, FALSE));
		pc->parsing.config = NULL;
	} else if (NULL != pc->parsing.luaconfig) {
		g_ptr_array_add(cmd, g_string_free(pc->parsing.luaconfig, FALSE));
		pc->parsing.luaconfig = NULL;
		g_ptr_array_add(cmd, g_strndup(CONST_STR_LEN("-l")));
	} else {
		g_ptr_array_add(cmd, g_strndup(CONST_STR_LEN("/etc/lighttpd2/lighttpd.conf")));
	}

	if (NULL != pc->parsing.modules_path) {
		g_ptr_array_add(cmd, g_strndup(CONST_STR_LEN("-m")));
		g_ptr_array_add(cmd, g_string_free(pc->parsing.modules_path, FALSE));
		pc->parsing.modules_path = NULL;
	}

	g_ptr_array_add(cmd, NULL);
	g_ptr_array_add(pc->parsing.env, NULL);
	cmdarr = (gchar**) g_ptr_array_free(cmd, FALSE);
	envarr = (gchar**) g_ptr_array_free(pc->parsing.env, FALSE);
	pc->parsing.env = g_ptr_array_new();

	user = pc->parsing.user;
	pc->parsing.user = NULL;

	pc->parsing.instconf = li_instance_conf_new(cmdarr, envarr, user, pc->parsing.user_uid, gid,
		pc->parsing.rlim_core, pc->parsing.rlim_nofile);

	return TRUE;
}


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

	LI_FORCE_ASSERT(g_atomic_int_get(&sock->refcount) > 0);
	if (g_atomic_int_dec_and_test(&sock->refcount)) {
		liPluginCoreConfig *config = (liPluginCoreConfig*) p->data;

		/* theoretically the hash table entry might not point to `sock`,
		 * but a) that shouldn't happen (can't bind two sockets to same
		 * address) and b) it doesn't matter - it just means the next
		 * `core_listen` will try to bind a new one (and fail...).
		 */
		g_hash_table_remove(config->listen_sockets, &sock->addr);

		li_sockaddr_clear(&sock->addr);
		close(sock->fd);

		g_slice_free(listen_socket, sock);
	}

	g_slice_free(listen_ref_resource, ref);
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

	switch (addr->addr_up.plain->sa_family) {
	case AF_INET: {
		struct sockaddr_in *ipv4 = addr->addr_up.ipv4;
		guint port = ntohs(ipv4->sin_port);

		if (config->listen_masks->len) {
			for (i = 0; i < config->listen_masks->len; i++) {
				mask = g_ptr_array_index(config->listen_masks, i);
				switch (mask->type) {
				case LI_PLUGIN_CORE_LISTEN_MASK_IPV4:
					if (!li_ipv4_in_ipv4_net(ipv4->sin_addr.s_addr, mask->value.ipv4.addr, mask->value.ipv4.networkmask)) continue;
					if ((mask->value.ipv4.port != port) && (mask->value.ipv4.port != 0 || (port != 80 && port != 443))) continue;
					return TRUE;
				/* strict matches only, no ipv4 in (ipv4-mapped) ipv6 */
				default:
					continue;
				}
			}
			return FALSE;
		} else {
			return (port == 80 || port == 443);
		}
	} break;
#ifdef HAVE_IPV6
	case AF_INET6: {
		struct sockaddr_in6 *ipv6 = addr->addr_up.ipv6;
		guint port = ntohs(ipv6->sin6_port);

		if (config->listen_masks->len) {
			for (i = 0; i < config->listen_masks->len; i++) {
				mask = g_ptr_array_index(config->listen_masks, i);
				switch (mask->type) {
				/* strict matches only, no (ipv4-mapped) ipv6 in ipv4 */
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
			return (port == 80 || port == 443);
		}
	} break;
#endif
#ifdef HAVE_SYS_UN_H
	case AF_UNIX: {
		if (config->listen_masks->len) {
			const gchar *fname = addr->addr_up.un->sun_path;

			for (i = 0; i < config->listen_masks->len; i++) {
				mask = g_ptr_array_index(config->listen_masks, i);
				switch (mask->type) {
				case LI_PLUGIN_CORE_LISTEN_MASK_UNIX:
					if (fnmatch(mask->value.unix_socket.path->str, fname, FNM_PERIOD | FNM_PATHNAME)) continue;
					return TRUE;
				default:
					continue;
				}
			}
			return FALSE;
		} else {
			return FALSE; /* don't allow unix by default */
		}
	} break;
#endif
	default:
		ERROR(srv, "Address family %i not supported", addr->addr_up.plain->sa_family);
		break;
	}
	return FALSE;
}

static int do_listen(liServer *srv, liSocketAddress *addr, GString *str) {
	int s, v;
	GString *ipv6_str;

	switch (addr->addr_up.plain->sa_family) {
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
		if (-1 == bind(s, addr->addr_up.plain, addr->len)) {
			close(s);
			ERROR(srv, "Couldn't bind socket to '%s': %s", str->str, g_strerror(errno));
			return -1;
		}
#ifdef TCP_FASTOPEN
		v = 1000;
		setsockopt(s, IPPROTO_TCP, TCP_FASTOPEN, &v, sizeof(v));
#endif
		if (-1 == listen(s, 1000)) {
			close(s);
			ERROR(srv, "Couldn't listen on '%s': %s", str->str, g_strerror(errno));
			return -1;
		}
		DEBUG(srv, "listen to ipv4: '%s' (port: %d)", str->str, ntohs(addr->addr_up.ipv4->sin_port));
		return s;
#ifdef HAVE_IPV6
	case AF_INET6:
		ipv6_str = g_string_sized_new(0);
		li_ipv6_tostring(ipv6_str, addr->addr_up.ipv6->sin6_addr.s6_addr);

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
		if (-1 == bind(s, addr->addr_up.plain, addr->len)) {
			close(s);
			ERROR(srv, "Couldn't bind socket to '%s': %s", ipv6_str->str, g_strerror(errno));
			g_string_free(ipv6_str, TRUE);
			return -1;
		}
#ifdef TCP_FASTOPEN
		v = 1000;
		setsockopt(s, IPPROTO_TCP, TCP_FASTOPEN, &v, sizeof(v));
#endif
		if (-1 == listen(s, 1000)) {
			close(s);
			ERROR(srv, "Couldn't listen on '%s': %s", ipv6_str->str, g_strerror(errno));
			g_string_free(ipv6_str, TRUE);
			return -1;
		}
		DEBUG(srv, "listen to ipv6: '%s' (port: %d)", ipv6_str->str, ntohs(addr->addr_up.ipv6->sin6_port));
		g_string_free(ipv6_str, TRUE);
		return s;
#endif
#ifdef HAVE_SYS_UN_H
	case AF_UNIX:
		if (-1 == unlink(addr->addr_up.un->sun_path)) {
			switch (errno) {
			case ENOENT:
				break;
			default:
				ERROR(srv, "removing old socket '%s' failed: %s\n", str->str, g_strerror(errno));
				return -1;
			}
		}
		if (-1 == (s = socket(AF_UNIX, SOCK_STREAM, 0))) {
			ERROR(srv, "Couldn't open socket: %s", g_strerror(errno));
			return -1;
		}
		if (-1 == bind(s, addr->addr_up.plain, addr->len)) {
			close(s);
			ERROR(srv, "Couldn't bind socket to '%s': %s", str->str, g_strerror(errno));
			return -1;
		}
		if (-1 == listen(s, 1000)) {
			close(s);
			ERROR(srv, "Couldn't listen on '%s': %s", str->str, g_strerror(errno));
			return -1;
		}
		DEBUG(srv, "listen to unix socket: '%s'", str->str);
		return s;
#endif
	default:
		ERROR(srv, "Address family %i not supported", addr->addr_up.plain->sa_family);
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

	/* DEBUG(srv, "core_listen(%i) '%s'", id, data->str); */

	if (-1 == id) return; /* ignore simple calls */

	addr = li_sockaddr_from_string(data, 80);
	if (!addr.addr_up.raw) {
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

static void core_log_open_file(liServer *srv, liPlugin *p, liInstance *i, gint32 id, GString *data) {
	GError *err = NULL;
	int fd = -1;
	GArray *fds;

	UNUSED(p);

	DEBUG(srv, "core_log_open_file(%i) '%s'", id, data->str);

	if (-1 == id) return; /* ignore simple calls */

	li_path_simplify(data);

	/* TODO: make path configurable */
	if (g_str_has_prefix(data->str, "/var/log/lighttpd2/")) {
		/* files can be read by everyone. if you don't like that, restrict access on the directory */
		/* if you need group write access for a specific group, use chmod g+s on the directory */
		/* "maybe-todo": add options for mode/owner/group */
		fd = open(data->str, O_RDWR | O_CREAT | O_APPEND, 0664);
		if (-1 == fd) {
			int e = errno;
			GString *error = g_string_sized_new(0);
			g_string_printf(error, "Couldn't open log file '%s': '%s'", data->str, g_strerror(e));

			ERROR(srv, "Couldn't open log file '%s': %s", data->str, g_strerror(e));

			if (!li_angel_send_result(i->acon, id, error, NULL, NULL, &err)) {
				ERROR(srv, "Couldn't send result: %s", err->message);
				g_error_free(err);
			}
			return;
		}
	} else {
		GString *error = g_string_sized_new(0);
		g_string_printf(error, "Couldn't open log file '%s': path not allowed", data->str);

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

static void core_free(liServer *srv, liPlugin *p) {
	liPluginCoreConfig *config = (liPluginCoreConfig*) p->data;
	guint i;

	li_event_clear(&config->sig_hup);

	core_parse_init(srv, p);
	g_ptr_array_free(config->parsing.env, TRUE);
	config->parsing.env = NULL;
	g_ptr_array_free(config->parsing.wrapper, TRUE);
	config->parsing.wrapper = NULL;
	g_ptr_array_free(config->parsing.listen_masks, TRUE);
	config->parsing.listen_masks = NULL;

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
	g_hash_table_destroy(config->listen_sockets);
	config->listen_masks = NULL;

	g_slice_free(liPluginCoreConfig, config);
}

static void core_activate(liServer *srv, liPlugin *p) {
	liPluginCoreConfig *config = (liPluginCoreConfig*) p->data;
	GPtrArray *tmp_ptrarray;
	guint i;

	if (NULL != config->instconf) {
		li_instance_conf_release(config->instconf);
		config->instconf = NULL;
	}

	if (NULL != config->inst) {
		li_instance_set_state(config->inst, LI_INSTANCE_FINISHED);
		li_instance_release(config->inst);
		config->inst = NULL;
	}

	for (i = 0; i < config->listen_masks->len; i++) {
		core_listen_mask_free(g_ptr_array_index(config->listen_masks, i));
	}
	g_ptr_array_set_size(config->listen_masks, 0);


	config->instconf = config->parsing.instconf;
	config->parsing.instconf = NULL;

	tmp_ptrarray = config->parsing.listen_masks; config->parsing.listen_masks = config->listen_masks; config->listen_masks = tmp_ptrarray;

	if (NULL != config->instconf) {
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

static void core_handle_sig_hup(liEventBase *watcher, int events) {
	liPluginCoreConfig *config = LI_CONTAINER_OF(li_event_signal_from(watcher), liPluginCoreConfig, sig_hup);
	liInstance *oldi, *newi;
	UNUSED(events);

	if (NULL == (oldi = config->inst)) return;

	if (oldi->replace_by) return;

	INFO(oldi->srv, "%s", "Received SIGHUP: graceful instance restart");
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
	p->handle_clean_config = core_parse_init;
	p->handle_check_config = core_check;
	p->handle_activate_config = core_activate;
	p->handle_instance_replaced = core_instance_replaced;

	core_parse_init(srv, p);
	config->listen_sockets = g_hash_table_new_full(li_hash_sockaddr, li_equal_sockaddr, NULL, NULL);
	config->listen_masks = g_ptr_array_new();

	li_angel_plugin_add_angel_cb(p, "listen", core_listen);
	li_angel_plugin_add_angel_cb(p, "reached-state", core_reached_state);
	li_angel_plugin_add_angel_cb(p, "log-open-file", core_log_open_file);

	li_event_signal_init(&srv->loop, "angel SIGHUP", &config->sig_hup, core_handle_sig_hup, SIGHUP);

	return TRUE;
}

gboolean li_plugin_core_init(liServer *srv) {
	/* load core plugins */
	return NULL != li_angel_plugin_register(srv, NULL, "core", core_init);
}

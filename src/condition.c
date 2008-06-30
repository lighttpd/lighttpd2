
#include "condition.h"
#include "log.h"

static condition* condition_find_cached(server *srv, GString *key);
static void condition_cache_insert(server *srv, GString *key, condition *c);
static condition* condition_new(config_cond_t cond, comp_key_t comp);
static condition* cond_new_string(config_cond_t cond, comp_key_t comp, GString *str);
static condition* cond_new_socket(config_cond_t cond, comp_key_t comp, GString *str);
static condition* condition_new_from_string(config_cond_t cond, comp_key_t comp, GString *str);
static void condition_free(condition *c);

static gboolean condition_check_eval(server *srv, connection *con, condition *cond);

static condition* condition_find_cached(server *srv, GString *key) {
	UNUSED(srv);
	UNUSED(key);

	return NULL;
}

static void condition_cache_insert(server *srv, GString *key, condition *c) {
	UNUSED(srv);
	UNUSED(c);

	g_string_free(key, TRUE);
}

static condition* condition_new(config_cond_t cond, comp_key_t comp) {
	condition *c = g_slice_new0(condition);
	c->refcount = 1;
	c->cache_index = -1;
	c->cond = cond;
	c->comp = comp;
	return c;
}

static condition* cond_new_string(config_cond_t cond, comp_key_t comp, GString *str) {
	condition *c = condition_new(cond, comp);
	switch (c->cond) {
	case CONFIG_COND_EQ:      /** == */
	case CONFIG_COND_NE:      /** != */
		c->value.string = str;
		break;
	case CONFIG_COND_MATCH:   /** =~ */
	case CONFIG_COND_NOMATCH: /** !~ */
#ifdef HAVE_PCRE_H
		/* TODO */
		ERROR("Regular expressions not supported for now in condition: %s %s '%s'",
			config_cond_to_string(cond), comp_key_to_string(comp),
			str);
		condition_free(c);
		return NULL;
		break;
#else
		ERROR("Regular expressions not supported in condition: %s %s '%s'",
			config_cond_to_string(cond), comp_key_to_string(comp),
			str->str);
		condition_free(c);
		return NULL;
#endif
	case CONFIG_COND_GT:      /** > */
	case CONFIG_COND_GE:      /** >= */
	case CONFIG_COND_LT:      /** < */
	case CONFIG_COND_LE:      /** <= */
		ERROR("Cannot compare with strings in condition: %s %s '%s'",
			config_cond_to_string(cond), comp_key_to_string(comp),
			str->str);
		condition_free(c);
		return NULL;
	}
	c->value_type = COND_VALUE_STRING;
	return c;
}

static condition* cond_new_socket(config_cond_t cond, comp_key_t comp, GString *str) {
	return cond_new_string(cond, comp, str);
	/* TODO: parse str as socket addr */
}

static condition* condition_new_from_string(config_cond_t cond, comp_key_t comp, GString *str) {
	switch (comp) {
	case COMP_SERVER_SOCKET:
	case COMP_REQUEST_REMOTE_IP:
		return cond_new_socket(cond, comp, str);
	case COMP_REQUEST_PATH:
	case COMP_REQUEST_HOST:
	case COMP_REQUEST_REFERER:
	case COMP_REQUEST_USER_AGENT:
	case COMP_REQUEST_COOKIE:
	case COMP_REQUEST_SCHEME:
	case COMP_REQUEST_QUERY_STRING:
	case COMP_REQUEST_METHOD:
	case COMP_PHYSICAL_PATH:
	case COMP_PHYSICAL_PATH_EXISTS:
		return cond_new_string(cond, comp, str);
	default:
		// TODO: die with error
		break;
	}
	return NULL;
}

condition* condition_new_string(server *srv, config_cond_t cond, comp_key_t comp, GString *str) {
	condition *c;
	GString *key = g_string_sized_new(0);
	g_string_sprintf(key, "%i:%i:%s", (int) cond, (int) comp, str->str);

	if (NULL != (c = condition_find_cached(srv, key))) {
		g_string_free(key, TRUE);
		return c;
	}

	c = condition_new_from_string(cond, comp, str);
	condition_cache_insert(srv, key, c);
	return c;
}

condition* condition_new_string_uncached(server *srv, config_cond_t cond, comp_key_t comp, GString *str) {
	condition *c;
	GString *key = g_string_sized_new(0);
	g_string_sprintf(key, "%i:%i:%s", (int) cond, (int) comp, str->str);

	c = condition_find_cached(srv, key);
	g_string_free(key, TRUE);
	if (NULL != c) return c;

	return condition_new_from_string(cond, comp, str);
}

static void condition_free(condition *c) {
	switch (c->value_type) {
	case COND_VALUE_INT:
	case COND_VALUE_SOCKET_IPV4:
	case COND_VALUE_SOCKET_IPV6:
		/* nothing to free */
		break;
	case COND_VALUE_STRING:
		if (c->cond == CONFIG_COND_MATCH || c->cond == CONFIG_COND_NOMATCH) {
#ifdef HAVE_PCRE_H
			if (c->value.regex) pcre_free(c->value.regex);
			if (c->value.regex_study) pcre_free(c->value.regex_study);
#endif
		} else {
			g_string_free(c->value.string, TRUE);
		}
		break;
	}
	g_slice_free(condition, c);
}

void condition_release(condition* c) {
	/* assert(c->recount > 0); */
	if (!(--c->refcount)) {
		condition_free(c);
	}
}

const char* config_cond_to_string(config_cond_t cond) {
	UNUSED(cond);

	/* TODO */
	return "";
}

const char* comp_key_to_string(comp_key_t comp) {
	UNUSED(comp);

	/* TODO */
	return "";
}

gboolean condition_check(server *srv, connection *con, condition *cond) {
	/* TODO: implement cache */
	return condition_check_eval(srv, con, cond);
}

static gboolean condition_check_eval_string(server *srv, connection *con, condition *cond) {
	const char *value = NULL;
	GString *tmp = NULL;
	gboolean result = FALSE;
	UNUSED(srv);
	UNUSED(con);

	switch (cond->comp) {
		/* TODO: get values */
	case COMP_SERVER_SOCKET:
		break;
	case COMP_REQUEST_PATH:
		value = con->request.uri.path->str;
		break;
	case COMP_REQUEST_HOST:
		value = con->request.host->str;
		break;
	case COMP_REQUEST_REFERER:
		break;
	case COMP_REQUEST_USER_AGENT:
		break;
	case COMP_REQUEST_COOKIE:
		break;
	case COMP_REQUEST_SCHEME:
		/* TODO: check for ssl */
		value = "http"; /* ssl ? "https" : "http" */
		break;
	case COMP_REQUEST_REMOTE_IP:
		value = con->dst_addr_str->str;
		break;
	case COMP_REQUEST_QUERY_STRING:
		value = con->request.uri.query->str;
		break;
	case COMP_REQUEST_METHOD:
		value = con->request.http_method_str->str;
		break;
	case COMP_PHYSICAL_PATH:
	case COMP_PHYSICAL_PATH_EXISTS:
		break;
	default:
		// TODO: die with error
		break;
	}

	if (value) switch (cond->cond) {
	case CONFIG_COND_EQ:      /** == */
		result = 0 == strcmp(value, cond->value.string->str);
		break;
	case CONFIG_COND_NE:      /** != */
		result = 0 != strcmp(value, cond->value.string->str);
		break;
	case CONFIG_COND_MATCH:   /** =~ */
	case CONFIG_COND_NOMATCH: /** !~ */
		/* TODO: pcre */
		break;
	default:
		break;
	}
	if (tmp) g_string_free(tmp, TRUE);
	return result;
}


static gboolean condition_check_eval_int(server *srv, connection *con, condition *cond) {
	UNUSED(srv);
	UNUSED(con);
	gint64 value;

	switch (cond->comp) {
		case COMP_REQUEST_SIZE:
			value = con->request.size;
		case COMP_PHYSICAL_SIZE:
			value = con->physical.size;
			break;
		default:
			value = -1;
	}

	if (value > 0) switch (cond->cond) {
		case CONFIG_COND_EQ:      /** == */
			return (value == cond->value.i);
		case CONFIG_COND_NE:      /** != */
			return (value != cond->value.i);
		case CONFIG_COND_LT:      /** < */
			return (value < cond->value.i);
		case CONFIG_COND_LE:      /** <= */
			return (value <= cond->value.i);
		case CONFIG_COND_GT:      /** > */
			return (value > cond->value.i);
		case CONFIG_COND_GE:      /** >= */
			return (value >= cond->value.i);
		default:
			// TODO: die with error
			return FALSE;
	}
}


static gboolean ipv4_in_ipv4_net(guint32 target, guint32 match, guint32 networkmask) {
	return (target & networkmask) == (match & networkmask);
}

#ifdef HAVE_IPV6
static gboolean ipv6_in_ipv6_net(const unsigned char *target, const guint8 *match, guint network) {
	guint8 mask = network % 8;
	if (0 != memcmp(target, match, network / 8)) return FALSE;
	if (!mask) return TRUE;
	mask = ~(((guint) 1 << (8-mask)) - 1);
	return (target[network / 8] & mask) == (match[network / 8] & mask);
}

static gboolean ipv6_in_ipv4_net(const unsigned char *target, guint32 match, guint32 networkmask) {
	static const guint8 ipv6match[] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF, 0, 0, 0, 0 };
	if (!ipv6_in_ipv6_net(target, ipv6match, 96)) return  FALSE;
	return ipv4_in_ipv4_net(*(guint32*)(target+12), match, networkmask);
}
#endif

static gboolean condition_check_eval(server *srv, connection *con, condition *cond) {
	switch (cond->value_type) {
		case COND_VALUE_STRING:
			return condition_check_eval_string(srv, con, cond);
		case COND_VALUE_INT:
			return condition_check_eval_int(srv, con, cond);
	/* TODO: implement checks */
		default:
			return FALSE;
	}
}

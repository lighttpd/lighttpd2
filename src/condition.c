#include "condition.h"
#include "log.h"

static condition* condition_new(comp_operator_t op, condition_lvalue *lvalue);
static condition* cond_new_string(comp_operator_t op, condition_lvalue *lvalue, GString *str);
static void condition_free(condition *c);

condition_lvalue* condition_lvalue_new(cond_lvalue_t type, GString *key) {
	condition_lvalue *lvalue = g_slice_new0(condition_lvalue);
	lvalue->type = type;
	lvalue->key = key;
	lvalue->refcount = 1;
	return lvalue;
}

void condition_lvalue_acquire(condition_lvalue *lvalue) {
	assert(lvalue->refcount > 0);
	lvalue->refcount++;
}

void condition_lvalue_release(condition_lvalue *lvalue) {
	assert(lvalue->refcount > 0);
	if (!(--lvalue->refcount)) {
		if (lvalue->key) g_string_free(lvalue->key, TRUE);
		g_slice_free(condition_lvalue, lvalue);
	}
}

static condition* condition_new(comp_operator_t op, condition_lvalue *lvalue) {
	condition *c = g_slice_new0(condition);
	c->refcount = 1;
	c->op = op;
	c->lvalue = lvalue;
	return c;
}

/* only EQ and NE */
static condition* cond_new_string(comp_operator_t op, condition_lvalue *lvalue, GString *str) {
	condition *c;
	c = condition_new(op, lvalue);
	c->rvalue.type = COND_VALUE_STRING;
	c->rvalue.string = str;
	return c;
}

#ifdef HAVE_PCRE_H
/* only MATCH and NOMATCH */
static condition* cond_new_match(server *srv, comp_operator_t op, condition_lvalue *lvalue, GString *str) {
	UNUSED(op); UNUSED(lvalue); UNUSED(str);
	ERROR(srv, "%s", "pcre not supported for now");
	/* TODO */
	return NULL;
}
#endif

/* only IP and NOTIP */
static condition* cond_new_ip(server *srv, comp_operator_t op, condition_lvalue *lvalue, GString *str) {
	UNUSED(op); UNUSED(lvalue); UNUSED(str);
	ERROR(srv, "%s", "ip matching not supported for now");
	/* TODO: parse str as socket addr */
	return NULL;
}

condition* condition_new_string(server *srv, comp_operator_t op, condition_lvalue *lvalue, GString *str) {
	switch (op) {
	case CONFIG_COND_EQ:
	case CONFIG_COND_NE:
		return cond_new_string(op, lvalue, str);
	case CONFIG_COND_MATCH:
	case CONFIG_COND_NOMATCH:
#ifdef HAVE_PCRE_H
		return cond_new_match(srv, op, lvalue, str);
#else
		ERROR(srv, "compiled without pcre, cannot use '%s'", comp_op_to_string(op));
		return NULL;
#endif
	case CONFIG_COND_IP:
	case CONFIG_COND_NOTIP:
		return cond_new_ip(srv, op, lvalue, str);
	case CONFIG_COND_GT:
	case CONFIG_COND_GE:
	case CONFIG_COND_LT:
	case CONFIG_COND_LE:
		ERROR(srv, "Cannot compare strings with '%s'", comp_op_to_string(op));
		return NULL;
	}
	ERROR(srv, "Condition creation failed: %s %s '%s' (perhaps you compiled without pcre?)",
		cond_lvalue_to_string(lvalue->type), comp_op_to_string(op),
		str->str);
	return NULL;
}

condition* condition_new_int(server *srv, comp_operator_t op, condition_lvalue *lvalue, gint64 i) {
	condition *c;
	switch (op) {
	case CONFIG_COND_MATCH:
	case CONFIG_COND_NOMATCH:
	case CONFIG_COND_IP:
	case CONFIG_COND_NOTIP:
		ERROR(srv, "Cannot compare integers with '%s'", comp_op_to_string(op));
		return NULL;
	case CONFIG_COND_EQ:
	case CONFIG_COND_NE:
	case CONFIG_COND_GT:
	case CONFIG_COND_GE:
	case CONFIG_COND_LT:
	case CONFIG_COND_LE:
		c = condition_new(op, lvalue);
		c->rvalue.type = COND_VALUE_INT;
		c->rvalue.i = i;
		return c;
	}
	ERROR(srv, "Condition creation failed: %s %s %"G_GINT64_FORMAT" (perhaps you compiled without pcre?)",
		cond_lvalue_to_string(lvalue->type), comp_op_to_string(op),
		i);
	return NULL;
}


static void condition_free(condition *c) {
	switch (c->rvalue.type) {
	case COND_VALUE_INT:
		/* nothing to free */
		break;
	case COND_VALUE_STRING:
		g_string_free(c->rvalue.string, TRUE);
		break;
#ifdef HAVE_PCRE_H
	case COND_VALUE_REGEXP
		if (c->rvalue.regex) pcre_free(c->rvalue.regex);
		if (c->rvalue.regex_study) pcre_free(c->rvalue.regex_study);
#endif
		break;
	case COND_VALUE_SOCKET_IPV4:
	case COND_VALUE_SOCKET_IPV6:
		/* nothing to free */
		break;
	}
	g_slice_free(condition, c);
}

void condition_acquire(condition *c) {
	assert(c->refcount > 0);
	c->refcount++;
}

void condition_release(server *srv, condition* c) {
	UNUSED(srv);
	/* assert(c->recount > 0); */
	if (!(--c->refcount)) {
		condition_free(c);
	}
}

const char* comp_op_to_string(comp_operator_t op) {
	switch (op) {
	case CONFIG_COND_EQ: return "==";
	case CONFIG_COND_NE: return "!=";
	case CONFIG_COND_MATCH: return "=~";
	case CONFIG_COND_NOMATCH: return "!~";
	case CONFIG_COND_IP: return "=/";
	case CONFIG_COND_NOTIP: return "!/";
	case CONFIG_COND_GT: return ">";
	case CONFIG_COND_GE: return ">=";
	case CONFIG_COND_LT: return "<";
	case CONFIG_COND_LE: return "<=";
	}

	return "<unkown>";
}

const char* cond_lvalue_to_string(cond_lvalue_t t) {
	switch (t) {
	case COMP_REQUEST_LOCALIP: return "request.localip";
	case COMP_REQUEST_REMOTEIP: return "request.remoteip";
	case COMP_REQUEST_PATH: return "request.path";
	case COMP_REQUEST_HOST: return "request.host";
	case COMP_REQUEST_SCHEME: return "request.scheme";
	case COMP_REQUEST_QUERY_STRING: return "request.querystring";
	case COMP_REQUEST_METHOD: return "request.method";
	case COMP_REQUEST_CONTENT_LENGTH: return "request.length";
	case COMP_PHYSICAL_PATH: return "physical.path";
	case COMP_PHYSICAL_PATH_EXISTS: return "physical.pathexist";
	case COMP_PHYSICAL_SIZE: return "physical.size";
	case COMP_REQUEST_HEADER: return "request.header";
	}

	return "<unknown>";
}

/* COND_VALUE_STRING and COND_VALUE_REGEXP only */
static gboolean condition_check_eval_string(server *srv, connection *con, condition *cond) {
	const char *value = NULL;
	GString *tmp = NULL;
	gboolean result = FALSE;
	UNUSED(srv);
	UNUSED(con);

	switch (cond->lvalue->type) {
		/* TODO: get values */
	case COMP_REQUEST_LOCALIP:
		break;
	case COMP_REQUEST_REMOTEIP:
		value = con->dst_addr_str->str;
		break;
	case COMP_REQUEST_PATH:
		value = con->request.uri.path->str;
		break;
	case COMP_REQUEST_HOST:
		value = con->request.host->str;
		break;
	case COMP_REQUEST_SCHEME:
		/* TODO: check for ssl */
		value = "http"; /* ssl ? "https" : "http" */
		break;
	case COMP_REQUEST_QUERY_STRING:
		value = con->request.uri.query->str;
		break;
	case COMP_REQUEST_METHOD:
		value = con->request.http_method_str->str;
		break;
	case COMP_PHYSICAL_PATH:
	case COMP_PHYSICAL_PATH_EXISTS:
		/* TODO */
		break;
	case COMP_REQUEST_HEADER:
		/* TODO */
		break;
	case COMP_PHYSICAL_SIZE:
	case COMP_REQUEST_CONTENT_LENGTH:
		// TODO: die with error
		assert(NULL);
		break;
	}

	if (value) switch (cond->op) {
	case CONFIG_COND_EQ:
		result = 0 == strcmp(value, cond->rvalue.string->str);
		break;
	case CONFIG_COND_NE:
		result = 0 != strcmp(value, cond->rvalue.string->str);
		break;
	case CONFIG_COND_MATCH:
	case CONFIG_COND_NOMATCH:
#ifdef HAVE_PCRE_H
		/* TODO: pcre */
		ERROR(srv, "%s", "regexp match not supported yet");
#else
		ERROR(srv, "compiled without pcre, cannot use '%s'", comp_op_to_string(cond->op));
#endif
		break;
	case CONFIG_COND_IP:
	case CONFIG_COND_NOTIP:
	case CONFIG_COND_GE:
	case CONFIG_COND_GT:
	case CONFIG_COND_LE:
	case CONFIG_COND_LT:
		ERROR(srv, "cannot compare string/regexp with '%s'", comp_op_to_string(cond->op));
		break;
	} else {
		ERROR(srv, "couldn't get string value for '%s'", cond_lvalue_to_string(cond->lvalue->type));
	}

	if (tmp) g_string_free(tmp, TRUE);
	return result;
}


static gboolean condition_check_eval_int(server *srv, connection *con, condition *cond) {
	UNUSED(srv);
	UNUSED(con);
	gint64 value;

	switch (cond->lvalue->type) {
	case COMP_REQUEST_CONTENT_LENGTH:
		value = con->request.content_length;
	case COMP_PHYSICAL_SIZE:
		value = con->physical.size;
		break;
	default:
		value = -1;
	}

	if (value > 0) switch (cond->op) {
	case CONFIG_COND_EQ:      /** == */
		return (value == cond->rvalue.i);
	case CONFIG_COND_NE:      /** != */
		return (value != cond->rvalue.i);
	case CONFIG_COND_LT:      /** < */
		return (value < cond->rvalue.i);
	case CONFIG_COND_LE:      /** <= */
		return (value <= cond->rvalue.i);
	case CONFIG_COND_GT:      /** > */
		return (value > cond->rvalue.i);
	case CONFIG_COND_GE:      /** >= */
		return (value >= cond->rvalue.i);
	case CONFIG_COND_MATCH:
	case CONFIG_COND_NOMATCH:
	case CONFIG_COND_IP:
	case CONFIG_COND_NOTIP:
		ERROR(srv, "cannot compare int with '%s'", comp_op_to_string(cond->op));
		return FALSE;
	} else {
		ERROR(srv, "couldn't get int value for '%s'", cond_lvalue_to_string(cond->lvalue->type));
	}

	return FALSE;
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

gboolean condition_check(server *srv, connection *con, condition *cond) {
	switch (cond->rvalue.type) {
	case COND_VALUE_STRING:
#ifdef HAVE_PCRE_H
	case COND_VALUE_REGEXP:
#endif
		return condition_check_eval_string(srv, con, cond);
	case COND_VALUE_INT:
		return condition_check_eval_int(srv, con, cond);
	case COND_VALUE_SOCKET_IPV4:
	case COND_VALUE_SOCKET_IPV6:
/* TODO: implement checks */
		break;
	}
	return FALSE;
}

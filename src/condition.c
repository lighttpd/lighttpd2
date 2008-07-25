#include "condition.h"
#include "log.h"

static condition* condition_new(comp_operator_t op, condition_lvalue *lvalue);
static condition* cond_new_string(comp_operator_t op, condition_lvalue *lvalue, GString *str);
static condition* cond_new_socket(comp_operator_t op, condition_lvalue *lvalue, GString *str);
static condition* condition_new_from_string(comp_operator_t op, condition_lvalue *lvalue, GString *str);
static void condition_free(condition *c);

static gboolean condition_check_eval(server *srv, connection *con, condition *cond);

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

static condition* cond_new_string(comp_operator_t op, condition_lvalue *lvalue, GString *str) {
	condition *c = condition_new(op, lvalue);
	switch (op) {
	case CONFIG_COND_EQ:      /** == */
	case CONFIG_COND_NE:      /** != */
		c->rvalue.string = str;
		break;
	case CONFIG_COND_MATCH:   /** =~ */
	case CONFIG_COND_NOMATCH: /** !~ */
#ifdef HAVE_PCRE_H
		/* TODO */
		condition_free(c);
		return NULL;
		break;
#else
		condition_free(c);
		return NULL;
#endif
	case CONFIG_COND_GT:      /** > */
	case CONFIG_COND_GE:      /** >= */
	case CONFIG_COND_LT:      /** < */
	case CONFIG_COND_LE:      /** <= */
		condition_free(c);
		return NULL;
	}
	c->rvalue.type = COND_VALUE_STRING;
	return c;
}

static condition* cond_new_socket(comp_operator_t op, condition_lvalue *lvalue, GString *str) {
	return cond_new_string(op, lvalue, str);
	/* TODO: parse str as socket addr */
}

static condition* condition_new_from_string(comp_operator_t op, condition_lvalue *lvalue, GString *str) {
	switch (lvalue->type) {
	case COMP_REQUEST_LOCALIP:
	case COMP_REQUEST_REMOTEIP:
		return cond_new_socket(op, lvalue, str);
	case COMP_REQUEST_PATH:
	case COMP_REQUEST_HOST:
	case COMP_REQUEST_SCHEME:
	case COMP_REQUEST_QUERY_STRING:
	case COMP_REQUEST_METHOD:
	case COMP_PHYSICAL_PATH:
	case COMP_PHYSICAL_PATH_EXISTS:
	case COMP_REQUEST_HEADER:
		return cond_new_string(op, lvalue, str);
	case COMP_PHYSICAL_SIZE:
	case COMP_REQUEST_CONTENT_LENGTH:
		// TODO: die with error
		assert(NULL);
		break;
	}
	return NULL;
}

condition* condition_new_string(server *srv, comp_operator_t op, condition_lvalue *lvalue, GString *str) {
	condition *c;

	if (NULL == (c = condition_new_from_string(op, lvalue, str))) {
		ERROR(srv, "Condition creation failed: %s %s '%s' (perhaps you compiled without pcre?)",
			cond_lvalue_to_string(lvalue->type), comp_op_to_string(op),
			str->str);
		return NULL;
	}

	return c;
}

static void condition_free(condition *c) {
	switch (c->rvalue.type) {
	case COND_VALUE_INT:
	case COND_VALUE_SOCKET_IPV4:
	case COND_VALUE_SOCKET_IPV6:
		/* nothing to free */
		break;
	case COND_VALUE_STRING:
		if (c->op == CONFIG_COND_MATCH || c->op == CONFIG_COND_NOMATCH) {
#ifdef HAVE_PCRE_H
			if (c->rvalue.regex) pcre_free(c->rvalue.regex);
			if (c->rvalue.regex_study) pcre_free(c->rvalue.regex_study);
#endif
		} else {
			g_string_free(c->rvalue.string, TRUE);
		}
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
	case CONFIG_COND_GE: return ">=";
	case CONFIG_COND_GT: return ">";
	case CONFIG_COND_LE: return "<=";
	case CONFIG_COND_LT: return "<";
	case CONFIG_COND_MATCH: return "=~";
	case CONFIG_COND_NE: return "!=";
	case CONFIG_COND_NOMATCH: return "!~";
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
	case CONFIG_COND_EQ:      /** == */
		result = 0 == strcmp(value, cond->rvalue.string->str);
		break;
	case CONFIG_COND_NE:      /** != */
		result = 0 != strcmp(value, cond->rvalue.string->str);
		break;
	case CONFIG_COND_MATCH:   /** =~ */
	case CONFIG_COND_NOMATCH: /** !~ */
		/* TODO: pcre */
		break;
	case CONFIG_COND_GE:
	case CONFIG_COND_GT:
	case CONFIG_COND_LE:
	case CONFIG_COND_LT:
		assert(NULL);
		break;
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
		// TODO: die with error
		assert(NULL);
		return FALSE;
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

static gboolean condition_check_eval(server *srv, connection *con, condition *cond) {
	switch (cond->rvalue.type) {
	case COND_VALUE_STRING:
		return condition_check_eval_string(srv, con, cond);
	case COND_VALUE_INT:
		return condition_check_eval_int(srv, con, cond);
/* TODO: implement checks */
	default:
		return FALSE;
	}
}

#include <lighttpd/base.h>

static gboolean condition_parse_ip(condition_rvalue *val, const char *txt) {
	if (parse_ipv4(txt, &val->ipv4.addr, NULL, NULL)) {
		val->type = COND_VALUE_SOCKET_IPV4;
		val->ipv4.networkmask = 0xFFFFFFFF;
		return TRUE;
	}
	if (parse_ipv6(txt, val->ipv6.addr, NULL, NULL)) {
		val->type = COND_VALUE_SOCKET_IPV6;
		val->ipv6.network = 128;
		return TRUE;
	}
	return FALSE;
}

static gboolean condition_parse_ip_net(condition_rvalue *val, const char *txt) {
	if (parse_ipv4(txt, &val->ipv4.addr, &val->ipv4.networkmask, NULL)) {
		val->type = COND_VALUE_SOCKET_IPV4;
		return TRUE;
	}
	if (parse_ipv6(txt, val->ipv6.addr, &val->ipv6.network, NULL)) {
		val->type = COND_VALUE_SOCKET_IPV6;
		return TRUE;
	}
	return FALSE;
}

static gboolean condition_ip_from_socket(condition_rvalue *val, sock_addr *addr) {
	switch (addr->plain.sa_family) {
	case AF_INET:
		val->type = COND_VALUE_SOCKET_IPV4;
		val->ipv4.addr = addr->ipv4.sin_addr.s_addr;
		val->ipv4.networkmask = 0xFFFFFFFF;
		return TRUE;
#ifdef HAVE_IPV6
	case AF_INET6:
		val->type = COND_VALUE_SOCKET_IPV6;
		memcpy(val->ipv6.addr, addr->ipv6.sin6_addr.s6_addr, 16);
		val->ipv6.network = 128;
		return TRUE;
#endif
	}
	return FALSE;
}

condition_lvalue* condition_lvalue_new(cond_lvalue_t type, GString *key) {
	condition_lvalue *lvalue = g_slice_new0(condition_lvalue);
	if (type == COMP_REQUEST_HEADER) g_string_ascii_down(key);
	lvalue->type = type;
	lvalue->key = key;
	lvalue->refcount = 1;
	return lvalue;
}

void condition_lvalue_acquire(condition_lvalue *lvalue) {
	assert(g_atomic_int_get(&lvalue->refcount) > 0);
	g_atomic_int_inc(&lvalue->refcount);
}

void condition_lvalue_release(condition_lvalue *lvalue) {
	if (!lvalue) return;
	assert(g_atomic_int_get(&lvalue->refcount) > 0);
	if (g_atomic_int_dec_and_test(&lvalue->refcount)) {
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
	/* TODO: pcre */
	return NULL;
}
#endif

/* only IP and NOTIP */
static condition* cond_new_ip(server *srv, comp_operator_t op, condition_lvalue *lvalue, GString *str) {
	condition *c;
	c = condition_new(op, lvalue);
	if (!condition_parse_ip_net(&c->rvalue, str->str)) {
		ERROR(srv, "Invalid ip address '%s'", str->str);
		condition_release(srv, c);
		return NULL;
	}
	return c;
}

condition* condition_new_string(server *srv, comp_operator_t op, condition_lvalue *lvalue, GString *str) {
	switch (op) {
	case CONFIG_COND_EQ:
	case CONFIG_COND_NE:
	case CONFIG_COND_PREFIX:
	case CONFIG_COND_NOPREFIX:
	case CONFIG_COND_SUFFIX:
	case CONFIG_COND_NOSUFFIX:
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
	case CONFIG_COND_PREFIX:
	case CONFIG_COND_NOPREFIX:
	case CONFIG_COND_SUFFIX:
	case CONFIG_COND_NOSUFFIX:
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
		c->rvalue.type = COND_VALUE_NUMBER;
		c->rvalue.i = i;
		return c;
	}
	ERROR(srv, "Condition creation failed: %s %s %"G_GINT64_FORMAT" (perhaps you compiled without pcre?)",
		cond_lvalue_to_string(lvalue->type), comp_op_to_string(op),
		i);
	return NULL;
}


static void condition_free(condition *c) {
	condition_lvalue_release(c->lvalue);
	switch (c->rvalue.type) {
	case COND_VALUE_NUMBER:
		/* nothing to free */
		break;
	case COND_VALUE_STRING:
		g_string_free(c->rvalue.string, TRUE);
		break;
#ifdef HAVE_PCRE_H
	case COND_VALUE_REGEXP:
		if (c->rvalue.pcre.regex) pcre_free(c->rvalue.pcre.regex);
		if (c->rvalue.pcre.regex_study) pcre_free(c->rvalue.pcre.regex_study);
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
	assert(g_atomic_int_get(&c->refcount) > 0);
	g_atomic_int_inc(&c->refcount);
}

void condition_release(server *srv, condition* c) {
	UNUSED(srv);
	if (!c) return;
	assert(g_atomic_int_get(&c->refcount) > 0);
	if (g_atomic_int_dec_and_test(&c->refcount)) {
		condition_free(c);
	}
}

const char* comp_op_to_string(comp_operator_t op) {
	switch (op) {
	case CONFIG_COND_EQ: return "==";
	case CONFIG_COND_NE: return "!=";
	case CONFIG_COND_PREFIX: return "=^";
	case CONFIG_COND_NOPREFIX: return "=^";
	case CONFIG_COND_SUFFIX: return "=$";
	case CONFIG_COND_NOSUFFIX: return "!$";
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
static handler_t condition_check_eval_string(vrequest *vr, condition *cond, gboolean *res) {
	connection *con = vr->con;
	const char *val = "";
	*res = FALSE;

	switch (cond->lvalue->type) {
	case COMP_REQUEST_LOCALIP:
		val = con->local_addr_str->str;
		break;
	case COMP_REQUEST_REMOTEIP:
		val = con->remote_addr_str->str;
		break;
	case COMP_REQUEST_PATH:
		val = vr->request.uri.path->str;
		break;
	case COMP_REQUEST_HOST:
		val = vr->request.uri.host->str;
		break;
	case COMP_REQUEST_SCHEME:
		val = con->is_ssl ? "https" : "http";
		break;
	case COMP_REQUEST_QUERY_STRING:
		val = vr->request.uri.query->str;
		break;
	case COMP_REQUEST_METHOD:
		val = vr->request.http_method_str->str;
		break;
	case COMP_PHYSICAL_PATH:
		val = vr->physical.path->str;
		break;
	case COMP_PHYSICAL_PATH_EXISTS:
		/* TODO: physical path exists */
		break;
	case COMP_REQUEST_HEADER:
		http_header_get_fast(con->wrk->tmp_str, vr->request.headers, GSTR_LEN(cond->lvalue->key));
		val = con->wrk->tmp_str->str;
		break;
	case COMP_PHYSICAL_SIZE:
		/* TODO: physical size */
		g_string_printf(con->wrk->tmp_str, "%"L_GOFFSET_FORMAT, (goffset) 0);
		val = con->wrk->tmp_str->str;
		break;
	case COMP_REQUEST_CONTENT_LENGTH:
		g_string_printf(con->wrk->tmp_str, "%"L_GOFFSET_FORMAT, vr->request.content_length);
		val = con->wrk->tmp_str->str;
		break;
	}

	switch (cond->op) {
	case CONFIG_COND_EQ:
		*res = g_str_equal(val, cond->rvalue.string->str);
		break;
	case CONFIG_COND_NE:
		*res = !g_str_equal(val, cond->rvalue.string->str);
		break;
	case CONFIG_COND_PREFIX:
		*res = g_str_has_prefix(val, cond->rvalue.string->str);
		break;
	case CONFIG_COND_NOPREFIX:
		*res = !g_str_has_prefix(val, cond->rvalue.string->str);
		break;
	case CONFIG_COND_SUFFIX:
		*res = g_str_has_suffix(val, cond->rvalue.string->str);
		break;
	case CONFIG_COND_NOSUFFIX:
		*res = !g_str_has_suffix(val, cond->rvalue.string->str);
		break;
	case CONFIG_COND_MATCH:
	case CONFIG_COND_NOMATCH:
#ifdef HAVE_PCRE_H
		/* TODO: pcre */
		VR_ERROR(vr, "%s", "regexp match not supported yet");
		return HANDLER_ERROR;
#else
		VR_ERROR(vr, "compiled without pcre, cannot use '%s'", comp_op_to_string(cond->op));
		return HANDLER_ERROR;
#endif
		break;
	case CONFIG_COND_IP:
	case CONFIG_COND_NOTIP:
	case CONFIG_COND_GE:
	case CONFIG_COND_GT:
	case CONFIG_COND_LE:
	case CONFIG_COND_LT:
		VR_ERROR(vr, "cannot compare string/regexp with '%s'", comp_op_to_string(cond->op));
		return HANDLER_ERROR;
	}

	return HANDLER_GO_ON;
}


static handler_t condition_check_eval_int(vrequest *vr, condition *cond, gboolean *res) {
	gint64 val;
	*res = FALSE;

	switch (cond->lvalue->type) {
	case COMP_REQUEST_CONTENT_LENGTH:
		val = vr->request.content_length;
	case COMP_PHYSICAL_SIZE:
		if (!vr->physical.have_stat) {
			if (!vrequest_stat(vr)) {
				switch (errno) {
				case EACCES: vr->response.http_status = 403; break;
				case EBADF: vr->response.http_status = 500; break;
				case EFAULT: vr->response.http_status = 500; break;
				case ELOOP: vr->response.http_status = 500; break;
				case ENAMETOOLONG: vr->response.http_status = 500; break;
				case ENOENT: vr->response.http_status = 404; break;
				case ENOMEM: vr->response.http_status = 500; break;
				case ENOTDIR: vr->response.http_status = 404; break;
				default: vr->response.http_status = 500;
				}
				vrequest_handle_direct(vr);
				return HANDLER_GO_ON;
			}
		}
		val = (gint64)vr->physical.stat.st_size;
		break;
	default:
		VR_ERROR(vr, "couldn't get int value for '%s'", cond_lvalue_to_string(cond->lvalue->type));
		return HANDLER_ERROR;
	}

	switch (cond->op) {
	case CONFIG_COND_EQ:      /** == */
		*res = (val == cond->rvalue.i);
		break;
	case CONFIG_COND_NE:      /** != */
		*res = (val != cond->rvalue.i);
		break;
	case CONFIG_COND_LT:      /** < */
		*res = (val < cond->rvalue.i);
		break;
	case CONFIG_COND_LE:      /** <= */
		*res = (val <= cond->rvalue.i);
		break;
	case CONFIG_COND_GT:      /** > */
		*res = (val > cond->rvalue.i);
		break;
	case CONFIG_COND_GE:      /** >= */
		*res = (val >= cond->rvalue.i);
		break;
	case CONFIG_COND_PREFIX:
	case CONFIG_COND_NOPREFIX:
	case CONFIG_COND_SUFFIX:
	case CONFIG_COND_NOSUFFIX:
	case CONFIG_COND_MATCH:
	case CONFIG_COND_NOMATCH:
	case CONFIG_COND_IP:
	case CONFIG_COND_NOTIP:
		VR_ERROR(vr, "cannot compare int with '%s'", comp_op_to_string(cond->op));
		return HANDLER_ERROR;
	}

	return HANDLER_GO_ON;
}

static gboolean ipv4_in_ipv4_net(guint32 target, guint32 match, guint32 networkmask) {
	return (target & networkmask) == (match & networkmask);
}

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

static gboolean ipv4_in_ipv6_net(guint32 target, const guint8 *match, guint network) {
	guint8 ipv6[] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF, 0, 0, 0, 0 };
	*(guint32*) (ipv6+12) = target;
	return ipv6_in_ipv6_net(ipv6, match, network);
}

static gboolean ip_in_net(condition_rvalue *target, condition_rvalue *network) {
	if (target->type == COND_VALUE_SOCKET_IPV4) {
		if (network->type == COND_VALUE_SOCKET_IPV4) {
			return ipv4_in_ipv4_net(target->ipv4.addr, network->ipv4.addr, network->ipv4.networkmask);
		} else if (network->type == COND_VALUE_SOCKET_IPV6) {
			return ipv4_in_ipv6_net(target->ipv4.addr, network->ipv6.addr, network->ipv6.network);
		}
	} else if (target->type == COND_VALUE_SOCKET_IPV6) {
		if (network->type == COND_VALUE_SOCKET_IPV4) {
			return ipv6_in_ipv4_net(target->ipv6.addr, network->ipv4.addr, network->ipv4.networkmask);
		} else if (network->type == COND_VALUE_SOCKET_IPV6) {
			return ipv6_in_ipv6_net(target->ipv6.addr, network->ipv6.addr, network->ipv6.network);
		}
	}
	return FALSE;
}

/* CONFIG_COND_IP and CONFIG_COND_NOTIP only */
static handler_t condition_check_eval_ip(vrequest *vr, condition *cond, gboolean *res) {
	connection *con = vr->con;
	condition_rvalue ipval;
	const char *val = NULL;
	*res = (cond->op == CONFIG_COND_NOTIP);

	ipval.type = COND_VALUE_NUMBER;

	switch (cond->lvalue->type) {
	case COMP_REQUEST_LOCALIP:
		if (!condition_ip_from_socket(&ipval, &con->local_addr))
			return HANDLER_GO_ON;
		break;
	case COMP_REQUEST_REMOTEIP:
		if (!condition_ip_from_socket(&ipval, &con->remote_addr))
			return HANDLER_GO_ON;
		break;
	case COMP_REQUEST_PATH:
		VR_ERROR(vr, "%s", "Cannot parse request.path as ip");
		return HANDLER_ERROR;
	case COMP_REQUEST_HOST:
		val = vr->request.uri.host->str;
		break;
	case COMP_REQUEST_SCHEME:
		VR_ERROR(vr, "%s", "Cannot parse request.scheme as ip");
		return HANDLER_ERROR;
	case COMP_REQUEST_QUERY_STRING:
		val = vr->request.uri.query->str;
		break;
	case COMP_REQUEST_METHOD:
		VR_ERROR(vr, "%s", "Cannot parse request.method as ip");
		return HANDLER_ERROR;
		break;
	case COMP_PHYSICAL_PATH:
	case COMP_PHYSICAL_PATH_EXISTS:
		VR_ERROR(vr, "%s", "Cannot parse physical.path(-exists) as ip");
		return HANDLER_ERROR;
		break;
	case COMP_REQUEST_HEADER:
		http_header_get_fast(con->wrk->tmp_str, vr->request.headers, GSTR_LEN(cond->lvalue->key));
		val = con->wrk->tmp_str->str;
		break;
	case COMP_PHYSICAL_SIZE:
	case COMP_REQUEST_CONTENT_LENGTH:
		VR_ERROR(vr, "%s", "Cannot parse integers as ip");
		return HANDLER_ERROR;
		break;
	}

	if (ipval.type == COND_VALUE_NUMBER) {
		if (!val || !condition_parse_ip(&ipval, val))
			return HANDLER_GO_ON;
	}

	switch (cond->op) {
	case CONFIG_COND_IP:
		*res = ip_in_net(&ipval, &cond->rvalue);
	case CONFIG_COND_NOTIP:
		*res = !ip_in_net(&ipval, &cond->rvalue);
	case CONFIG_COND_PREFIX:
	case CONFIG_COND_NOPREFIX:
	case CONFIG_COND_SUFFIX:
	case CONFIG_COND_NOSUFFIX:
	case CONFIG_COND_EQ:
	case CONFIG_COND_NE:
	case CONFIG_COND_MATCH:
	case CONFIG_COND_NOMATCH:
	case CONFIG_COND_GE:
	case CONFIG_COND_GT:
	case CONFIG_COND_LE:
	case CONFIG_COND_LT:
		VR_ERROR(vr, "cannot match ips with '%s'", comp_op_to_string(cond->op));
		return HANDLER_ERROR;
	}

	return HANDLER_GO_ON;
}

handler_t condition_check(vrequest *vr, condition *cond, gboolean *res) {
	switch (cond->rvalue.type) {
	case COND_VALUE_STRING:
#ifdef HAVE_PCRE_H
	case COND_VALUE_REGEXP:
#endif
		return condition_check_eval_string(vr, cond, res);
	case COND_VALUE_NUMBER:
		return condition_check_eval_int(vr, cond, res);
	case COND_VALUE_SOCKET_IPV4:
	case COND_VALUE_SOCKET_IPV6:
		return condition_check_eval_ip(vr, cond, res);
	}
	VR_ERROR(vr, "Unsupported conditional type: %i", cond->rvalue.type);
	return HANDLER_ERROR;
}

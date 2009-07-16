#include <lighttpd/base.h>

static gboolean condition_parse_ip(liConditionRValue *val, const char *txt) {
	if (li_parse_ipv4(txt, &val->ipv4.addr, NULL, NULL)) {
		val->type = LI_COND_VALUE_SOCKET_IPV4;
		val->ipv4.networkmask = 0xFFFFFFFF;
		return TRUE;
	}
	if (li_parse_ipv6(txt, val->ipv6.addr, NULL, NULL)) {
		val->type = LI_COND_VALUE_SOCKET_IPV6;
		val->ipv6.network = 128;
		return TRUE;
	}
	return FALSE;
}

static gboolean condition_parse_ip_net(liConditionRValue *val, const char *txt) {
	if (li_parse_ipv4(txt, &val->ipv4.addr, &val->ipv4.networkmask, NULL)) {
		val->type = LI_COND_VALUE_SOCKET_IPV4;
		return TRUE;
	}
	if (li_parse_ipv6(txt, val->ipv6.addr, &val->ipv6.network, NULL)) {
		val->type = LI_COND_VALUE_SOCKET_IPV6;
		return TRUE;
	}
	return FALSE;
}

static gboolean condition_ip_from_socket(liConditionRValue *val, liSockAddr *addr) {
	switch (addr->plain.sa_family) {
	case AF_INET:
		val->type = LI_COND_VALUE_SOCKET_IPV4;
		val->ipv4.addr = addr->ipv4.sin_addr.s_addr;
		val->ipv4.networkmask = 0xFFFFFFFF;
		return TRUE;
#ifdef HAVE_IPV6
	case AF_INET6:
		val->type = LI_COND_VALUE_SOCKET_IPV6;
		memcpy(val->ipv6.addr, addr->ipv6.sin6_addr.s6_addr, 16);
		val->ipv6.network = 128;
		return TRUE;
#endif
	}
	return FALSE;
}

liConditionLValue* li_condition_lvalue_new(liCondLValue type, GString *key) {
	liConditionLValue *lvalue = g_slice_new0(liConditionLValue);
	if (type == LI_COMP_REQUEST_HEADER) g_string_ascii_down(key);
	lvalue->type = type;
	lvalue->key = key;
	lvalue->refcount = 1;
	return lvalue;
}

void li_condition_lvalue_acquire(liConditionLValue *lvalue) {
	assert(g_atomic_int_get(&lvalue->refcount) > 0);
	g_atomic_int_inc(&lvalue->refcount);
}

void li_condition_lvalue_release(liConditionLValue *lvalue) {
	if (!lvalue) return;
	assert(g_atomic_int_get(&lvalue->refcount) > 0);
	if (g_atomic_int_dec_and_test(&lvalue->refcount)) {
		if (lvalue->key) g_string_free(lvalue->key, TRUE);
		g_slice_free(liConditionLValue, lvalue);
	}
}

static liCondition* condition_new(liCompOperator op, liConditionLValue *lvalue) {
	liCondition *c = g_slice_new0(liCondition);
	c->refcount = 1;
	c->op = op;
	c->lvalue = lvalue;
	return c;
}

/* only EQ and NE */
static liCondition* cond_new_string(liCompOperator op, liConditionLValue *lvalue, GString *str) {
	liCondition *c;
	c = condition_new(op, lvalue);
	c->rvalue.type = LI_COND_VALUE_STRING;
	c->rvalue.string = str;
	return c;
}

/* only MATCH and NOMATCH */
static liCondition* cond_new_match(liServer *srv, liCompOperator op, liConditionLValue *lvalue, GString *str) {
	liCondition *c;
	GRegex *regex;
	GError *err = NULL;

	regex = g_regex_new(str->str, G_REGEX_RAW | G_REGEX_OPTIMIZE, 0, &err);

	if (!regex || err) {
		ERROR(srv, "failed to compile regex \"%s\": %s", str->str, err->message);
		g_error_free(err);
		return NULL;
	}

	c = condition_new(op, lvalue);
	c->rvalue.type = LI_COND_VALUE_REGEXP;
	c->rvalue.regex = regex;

	g_string_free(str, TRUE);

	return c;
}

/* only IP and NOTIP */
static liCondition* cond_new_ip(liServer *srv, liCompOperator op, liConditionLValue *lvalue, GString *str) {
	liCondition *c;
	c = condition_new(op, lvalue);
	if (!condition_parse_ip_net(&c->rvalue, str->str)) {
		ERROR(srv, "Invalid ip address '%s'", str->str);
		li_condition_release(srv, c);
		return NULL;
	}
	return c;
}

liCondition* li_condition_new_bool(liServer *srv, liConditionLValue *lvalue, gboolean b) {
	liCondition *c;
	UNUSED(srv);
	c = condition_new(LI_CONFIG_COND_EQ, lvalue);
	c->rvalue.type = LI_COND_VALUE_BOOL;
	c->rvalue.b = b;
	return c;
}

liCondition* li_condition_new_string(liServer *srv, liCompOperator op, liConditionLValue *lvalue, GString *str) {
	switch (op) {
	case LI_CONFIG_COND_EQ:
	case LI_CONFIG_COND_NE:
	case LI_CONFIG_COND_PREFIX:
	case LI_CONFIG_COND_NOPREFIX:
	case LI_CONFIG_COND_SUFFIX:
	case LI_CONFIG_COND_NOSUFFIX:
		return cond_new_string(op, lvalue, str);
	case LI_CONFIG_COND_MATCH:
	case LI_CONFIG_COND_NOMATCH:
		return cond_new_match(srv, op, lvalue, str);
	case LI_CONFIG_COND_IP:
	case LI_CONFIG_COND_NOTIP:
		return cond_new_ip(srv, op, lvalue, str);
	case LI_CONFIG_COND_GT:
	case LI_CONFIG_COND_GE:
	case LI_CONFIG_COND_LT:
	case LI_CONFIG_COND_LE:
		ERROR(srv, "Cannot compare strings with '%s'", li_comp_op_to_string(op));
		return NULL;
	}
	ERROR(srv, "Condition creation failed: %s %s '%s'",
		li_cond_lvalue_to_string(lvalue->type), li_comp_op_to_string(op),
		str->str);
	return NULL;
}

liCondition* li_condition_new_int(liServer *srv, liCompOperator op, liConditionLValue *lvalue, gint64 i) {
	liCondition *c;
	switch (op) {
	case LI_CONFIG_COND_PREFIX:
	case LI_CONFIG_COND_NOPREFIX:
	case LI_CONFIG_COND_SUFFIX:
	case LI_CONFIG_COND_NOSUFFIX:
	case LI_CONFIG_COND_MATCH:
	case LI_CONFIG_COND_NOMATCH:
	case LI_CONFIG_COND_IP:
	case LI_CONFIG_COND_NOTIP:
		ERROR(srv, "Cannot compare integers with '%s'", li_comp_op_to_string(op));
		return NULL;
	case LI_CONFIG_COND_EQ:
	case LI_CONFIG_COND_NE:
	case LI_CONFIG_COND_GT:
	case LI_CONFIG_COND_GE:
	case LI_CONFIG_COND_LT:
	case LI_CONFIG_COND_LE:
		c = condition_new(op, lvalue);
		c->rvalue.type = LI_COND_VALUE_NUMBER;
		c->rvalue.i = i;
		return c;
	}
	ERROR(srv, "Condition creation failed: %s %s %"G_GINT64_FORMAT,
		li_cond_lvalue_to_string(lvalue->type), li_comp_op_to_string(op),
		i);
	return NULL;
}


static void condition_free(liCondition *c) {
	li_condition_lvalue_release(c->lvalue);
	switch (c->rvalue.type) {
	case LI_COND_VALUE_BOOL:
	case LI_COND_VALUE_NUMBER:
		/* nothing to free */
		break;
	case LI_COND_VALUE_STRING:
		g_string_free(c->rvalue.string, TRUE);
		break;
	case LI_COND_VALUE_REGEXP:
		g_regex_unref(c->rvalue.regex);
		break;
	case LI_COND_VALUE_SOCKET_IPV4:
	case LI_COND_VALUE_SOCKET_IPV6:
		/* nothing to free */
		break;
	}
	g_slice_free(liCondition, c);
}

void li_condition_acquire(liCondition *c) {
	assert(g_atomic_int_get(&c->refcount) > 0);
	g_atomic_int_inc(&c->refcount);
}

void li_condition_release(liServer *srv, liCondition* c) {
	UNUSED(srv);
	if (!c) return;
	assert(g_atomic_int_get(&c->refcount) > 0);
	if (g_atomic_int_dec_and_test(&c->refcount)) {
		condition_free(c);
	}
}

const char* li_comp_op_to_string(liCompOperator op) {
	switch (op) {
	case LI_CONFIG_COND_EQ: return "==";
	case LI_CONFIG_COND_NE: return "!=";
	case LI_CONFIG_COND_PREFIX: return "=^";
	case LI_CONFIG_COND_NOPREFIX: return "=^";
	case LI_CONFIG_COND_SUFFIX: return "=$";
	case LI_CONFIG_COND_NOSUFFIX: return "!$";
	case LI_CONFIG_COND_MATCH: return "=~";
	case LI_CONFIG_COND_NOMATCH: return "!~";
	case LI_CONFIG_COND_IP: return "=/";
	case LI_CONFIG_COND_NOTIP: return "!/";
	case LI_CONFIG_COND_GT: return ">";
	case LI_CONFIG_COND_GE: return ">=";
	case LI_CONFIG_COND_LT: return "<";
	case LI_CONFIG_COND_LE: return "<=";
	}

	return "<unkown>";
}

const char* li_cond_lvalue_to_string(liCondLValue t) {
	switch (t) {
	case LI_COMP_REQUEST_LOCALIP: return "request.localip";
	case LI_COMP_REQUEST_REMOTEIP: return "request.remoteip";
	case LI_COMP_REQUEST_PATH: return "request.path";
	case LI_COMP_REQUEST_HOST: return "request.host";
	case LI_COMP_REQUEST_SCHEME: return "request.scheme";
	case LI_COMP_REQUEST_QUERY_STRING: return "request.query";
	case LI_COMP_REQUEST_METHOD: return "request.method";
	case LI_COMP_REQUEST_CONTENT_LENGTH: return "request.length";
	case LI_COMP_PHYSICAL_PATH: return "physical.path";
	case LI_COMP_PHYSICAL_PATH_EXISTS: return "physical.exist";
	case LI_COMP_PHYSICAL_SIZE: return "physical.size";
	case LI_COMP_PHYSICAL_ISDIR: return "physical.is_dir";
	case LI_COMP_PHYSICAL_ISFILE: return "physical.is_file";
	case LI_COMP_REQUEST_HEADER: return "request.header";
	case LI_COMP_UNKNOWN: return "<unknown>";
	}

	return "<unkown>";
}

liCondLValue li_cond_lvalue_from_string(const gchar *str, guint len) {
	gchar *c = (gchar*)str;

	if (g_str_has_prefix(c, "request.")) {
		c += sizeof("request.")-1;
		len -= sizeof("request.")-1;

		if (strncmp(c, "localip", len) == 0)
			return LI_COMP_REQUEST_LOCALIP;
		else if (strncmp(c, "remoteip", len) == 0)
			return LI_COMP_REQUEST_REMOTEIP;
		else if (strncmp(c, "path", len) == 0)
			return LI_COMP_REQUEST_PATH;
		else if (strncmp(c, "host", len) == 0)
			return LI_COMP_REQUEST_HOST;
		else if (strncmp(c, "scheme", len) == 0)
			return LI_COMP_REQUEST_SCHEME;
		else if (strncmp(c, "query", len) == 0)
			return LI_COMP_REQUEST_QUERY_STRING;
		else if (strncmp(c, "method", len) == 0)
			return LI_COMP_REQUEST_METHOD;
		else if (strncmp(c, "length", len) == 0)
			return LI_COMP_REQUEST_CONTENT_LENGTH;
		else if (strncmp(c, "header", len) == 0)
			return LI_COMP_REQUEST_HEADER;
	} else if (strncmp(c, "physical.", sizeof("physical.")-1) == 0) {
		c += sizeof("physical.")-1;
		len -= sizeof("physical.")-1;

		if (strncmp(c, "path", len) == 0)
			return LI_COMP_PHYSICAL_PATH;
		else if (strncmp(c, "exists", len) == 0)
			return LI_COMP_PHYSICAL_PATH_EXISTS;
		else if (strncmp(c, "size", len) == 0)
			return LI_COMP_PHYSICAL_SIZE;
		else if (strncmp(c, "is_file", len) == 0)
			return LI_COMP_PHYSICAL_ISFILE;
		else if (strncmp(c, "is_dir", len) == 0)
			return LI_COMP_PHYSICAL_ISDIR;
	}

	return LI_COMP_UNKNOWN;
}

static liHandlerResult li_condition_check_eval_bool(liVRequest *vr, liCondition *cond, gboolean *res) {
	*res = FALSE;

	if (cond->lvalue->type == LI_COMP_PHYSICAL_ISDIR ||
		cond->lvalue->type == LI_COMP_PHYSICAL_ISFILE) {
		if (!vr->physical.have_stat) {
			if (vr->physical.have_errno || !li_vrequest_stat(vr)) {
				switch (vr->physical.stat_errno) {
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
				li_vrequest_handle_direct(vr);
				return LI_HANDLER_GO_ON;
			}
		}
	}

	switch (cond->lvalue->type) {
	case LI_COMP_PHYSICAL_ISDIR:
		*res = S_ISDIR(vr->physical.stat.st_mode);
		break;
	case LI_COMP_PHYSICAL_ISFILE:
		*res = S_ISREG(vr->physical.stat.st_mode);
		break;
	default:
		VR_ERROR(vr, "invalid lvalue \"%s\" for boolean comparison", li_cond_lvalue_to_string(cond->lvalue->type));
		return LI_HANDLER_ERROR;
	}

	return LI_HANDLER_GO_ON;
}

/* LI_COND_VALUE_STRING and LI_COND_VALUE_REGEXP only */
static liHandlerResult li_condition_check_eval_string(liVRequest *vr, liCondition *cond, gboolean *res) {
	liActionRegexStackElement arse;
	liConnection *con = vr->con;
	const char *val = "";
	*res = FALSE;

	switch (cond->lvalue->type) {
	case LI_COMP_REQUEST_LOCALIP:
		val = con->srv_sock->local_addr_str->str;
		break;
	case LI_COMP_REQUEST_REMOTEIP:
		val = con->remote_addr_str->str;
		break;
	case LI_COMP_REQUEST_PATH:
		val = vr->request.uri.path->str;
		break;
	case LI_COMP_REQUEST_HOST:
		val = vr->request.uri.host->str;
		break;
	case LI_COMP_REQUEST_SCHEME:
		val = con->is_ssl ? "https" : "http";
		break;
	case LI_COMP_REQUEST_QUERY_STRING:
		val = vr->request.uri.query->str;
		break;
	case LI_COMP_REQUEST_METHOD:
		val = vr->request.http_method_str->str;
		break;
	case LI_COMP_PHYSICAL_PATH:
		val = vr->physical.path->str;
		break;
	case LI_COMP_PHYSICAL_PATH_EXISTS:
		/* TODO: physical path exists */
		break;
	case LI_COMP_REQUEST_HEADER:
		li_http_header_get_fast(con->wrk->tmp_str, vr->request.headers, GSTR_LEN(cond->lvalue->key));
		val = con->wrk->tmp_str->str;
		break;
	case LI_COMP_REQUEST_CONTENT_LENGTH:
		g_string_printf(con->wrk->tmp_str, "%"L_GOFFSET_FORMAT, vr->request.content_length);
		val = con->wrk->tmp_str->str;
		break;
	default:
		return LI_HANDLER_ERROR;
	}

	switch (cond->op) {
	case LI_CONFIG_COND_EQ:
		*res = g_str_equal(val, cond->rvalue.string->str);
		break;
	case LI_CONFIG_COND_NE:
		*res = !g_str_equal(val, cond->rvalue.string->str);
		break;
	case LI_CONFIG_COND_PREFIX:
		*res = g_str_has_prefix(val, cond->rvalue.string->str);
		break;
	case LI_CONFIG_COND_NOPREFIX:
		*res = !g_str_has_prefix(val, cond->rvalue.string->str);
		break;
	case LI_CONFIG_COND_SUFFIX:
		*res = g_str_has_suffix(val, cond->rvalue.string->str);
		break;
	case LI_CONFIG_COND_NOSUFFIX:
		*res = !g_str_has_suffix(val, cond->rvalue.string->str);
		break;
	case LI_CONFIG_COND_MATCH:
		*res = g_regex_match(cond->rvalue.regex, val, 0, &arse.match_info);
		arse.string = (*res) ? g_string_new(val) : NULL;
		g_array_append_val(vr->action_stack.regex_stack, arse);
		break;
	case LI_CONFIG_COND_NOMATCH:
		*res = !g_regex_match(cond->rvalue.regex, val, 0, &arse.match_info);
		arse.string = NULL;
		g_array_append_val(vr->action_stack.regex_stack, arse);
		break;
	case LI_CONFIG_COND_IP:
	case LI_CONFIG_COND_NOTIP:
	case LI_CONFIG_COND_GE:
	case LI_CONFIG_COND_GT:
	case LI_CONFIG_COND_LE:
	case LI_CONFIG_COND_LT:
		VR_ERROR(vr, "cannot compare string/regexp with '%s'", li_comp_op_to_string(cond->op));
		return LI_HANDLER_ERROR;
	}

	return LI_HANDLER_GO_ON;
}


static liHandlerResult li_condition_check_eval_int(liVRequest *vr, liCondition *cond, gboolean *res) {
	gint64 val;
	*res = FALSE;

	switch (cond->lvalue->type) {
	case LI_COMP_REQUEST_CONTENT_LENGTH:
		val = vr->request.content_length;
		break;
	case LI_COMP_PHYSICAL_SIZE:
		if (!vr->physical.have_stat) {
			if (vr->physical.have_errno || !li_vrequest_stat(vr)) {
				switch (vr->physical.stat_errno) {
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
				li_vrequest_handle_direct(vr);
				return LI_HANDLER_GO_ON;
			}
		}
		val = (gint64)vr->physical.stat.st_size;
		break;
	default:
		VR_ERROR(vr, "couldn't get int value for '%s'", li_cond_lvalue_to_string(cond->lvalue->type));
		return LI_HANDLER_ERROR;
	}

	switch (cond->op) {
	case LI_CONFIG_COND_EQ:      /** == */
		*res = (val == cond->rvalue.i);
		break;
	case LI_CONFIG_COND_NE:      /** != */
		*res = (val != cond->rvalue.i);
		break;
	case LI_CONFIG_COND_LT:      /** < */
		*res = (val < cond->rvalue.i);
		break;
	case LI_CONFIG_COND_LE:      /** <= */
		*res = (val <= cond->rvalue.i);
		break;
	case LI_CONFIG_COND_GT:      /** > */
		*res = (val > cond->rvalue.i);
		break;
	case LI_CONFIG_COND_GE:      /** >= */
		*res = (val >= cond->rvalue.i);
		break;
	case LI_CONFIG_COND_PREFIX:
	case LI_CONFIG_COND_NOPREFIX:
	case LI_CONFIG_COND_SUFFIX:
	case LI_CONFIG_COND_NOSUFFIX:
	case LI_CONFIG_COND_MATCH:
	case LI_CONFIG_COND_NOMATCH:
	case LI_CONFIG_COND_IP:
	case LI_CONFIG_COND_NOTIP:
		VR_ERROR(vr, "cannot compare int with '%s'", li_comp_op_to_string(cond->op));
		return LI_HANDLER_ERROR;
	}

	return LI_HANDLER_GO_ON;
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

static gboolean ip_in_net(liConditionRValue *target, liConditionRValue *network) {
	if (target->type == LI_COND_VALUE_SOCKET_IPV4) {
		if (network->type == LI_COND_VALUE_SOCKET_IPV4) {
			return ipv4_in_ipv4_net(target->ipv4.addr, network->ipv4.addr, network->ipv4.networkmask);
		} else if (network->type == LI_COND_VALUE_SOCKET_IPV6) {
			return ipv4_in_ipv6_net(target->ipv4.addr, network->ipv6.addr, network->ipv6.network);
		}
	} else if (target->type == LI_COND_VALUE_SOCKET_IPV6) {
		if (network->type == LI_COND_VALUE_SOCKET_IPV4) {
			return ipv6_in_ipv4_net(target->ipv6.addr, network->ipv4.addr, network->ipv4.networkmask);
		} else if (network->type == LI_COND_VALUE_SOCKET_IPV6) {
			return ipv6_in_ipv6_net(target->ipv6.addr, network->ipv6.addr, network->ipv6.network);
		}
	}
	return FALSE;
}

/* LI_CONFIG_COND_IP and LI_CONFIG_COND_NOTIP only */
static liHandlerResult li_condition_check_eval_ip(liVRequest *vr, liCondition *cond, gboolean *res) {
	liConnection *con = vr->con;
	liConditionRValue ipval;
	const char *val = NULL;
	*res = (cond->op == LI_CONFIG_COND_NOTIP);

	ipval.type = LI_COND_VALUE_NUMBER;

	switch (cond->lvalue->type) {
	case LI_COMP_REQUEST_LOCALIP:
		if (!condition_ip_from_socket(&ipval, con->srv_sock->local_addr.addr))
			return LI_HANDLER_GO_ON;
		break;
	case LI_COMP_REQUEST_REMOTEIP:
		if (!condition_ip_from_socket(&ipval, con->remote_addr.addr))
			return LI_HANDLER_GO_ON;
		break;
	case LI_COMP_REQUEST_PATH:
		VR_ERROR(vr, "%s", "Cannot parse request.path as ip");
		return LI_HANDLER_ERROR;
	case LI_COMP_REQUEST_HOST:
		val = vr->request.uri.host->str;
		break;
	case LI_COMP_REQUEST_SCHEME:
		VR_ERROR(vr, "%s", "Cannot parse request.scheme as ip");
		return LI_HANDLER_ERROR;
	case LI_COMP_REQUEST_QUERY_STRING:
		val = vr->request.uri.query->str;
		break;
	case LI_COMP_REQUEST_METHOD:
		VR_ERROR(vr, "%s", "Cannot parse request.method as ip");
		return LI_HANDLER_ERROR;
		break;
	case LI_COMP_PHYSICAL_PATH:
	case LI_COMP_PHYSICAL_PATH_EXISTS:
		VR_ERROR(vr, "%s", "Cannot parse physical.path(-exists) as ip");
		return LI_HANDLER_ERROR;
		break;
	case LI_COMP_REQUEST_HEADER:
		li_http_header_get_fast(con->wrk->tmp_str, vr->request.headers, GSTR_LEN(cond->lvalue->key));
		val = con->wrk->tmp_str->str;
		break;
	case LI_COMP_PHYSICAL_SIZE:
	case LI_COMP_REQUEST_CONTENT_LENGTH:
		VR_ERROR(vr, "%s", "Cannot parse integers as ip");
		return LI_HANDLER_ERROR;
	case LI_COMP_PHYSICAL_ISDIR:
	case LI_COMP_PHYSICAL_ISFILE:
		VR_ERROR(vr, "%s", "phys.is_dir and phys.is_file are boolean conditionals");
		return LI_HANDLER_ERROR;
	case LI_COMP_UNKNOWN:
		VR_ERROR(vr, "%s", "Cannot parse unknown condition value");
		return LI_HANDLER_ERROR;
	}

	if (ipval.type == LI_COND_VALUE_NUMBER) {
		if (!val || !condition_parse_ip(&ipval, val))
			return LI_HANDLER_GO_ON;
	}

	switch (cond->op) {
	case LI_CONFIG_COND_IP:
		*res = ip_in_net(&ipval, &cond->rvalue);
	case LI_CONFIG_COND_NOTIP:
		*res = !ip_in_net(&ipval, &cond->rvalue);
	case LI_CONFIG_COND_PREFIX:
	case LI_CONFIG_COND_NOPREFIX:
	case LI_CONFIG_COND_SUFFIX:
	case LI_CONFIG_COND_NOSUFFIX:
	case LI_CONFIG_COND_EQ:
	case LI_CONFIG_COND_NE:
	case LI_CONFIG_COND_MATCH:
	case LI_CONFIG_COND_NOMATCH:
	case LI_CONFIG_COND_GE:
	case LI_CONFIG_COND_GT:
	case LI_CONFIG_COND_LE:
	case LI_CONFIG_COND_LT:
		VR_ERROR(vr, "cannot match ips with '%s'", li_comp_op_to_string(cond->op));
		return LI_HANDLER_ERROR;
	}

	return LI_HANDLER_GO_ON;
}

liHandlerResult li_condition_check(liVRequest *vr, liCondition *cond, gboolean *res) {
	switch (cond->rvalue.type) {
	case LI_COND_VALUE_BOOL:
		return li_condition_check_eval_bool(vr, cond, res);
	case LI_COND_VALUE_STRING:
	case LI_COND_VALUE_REGEXP:
		return li_condition_check_eval_string(vr, cond, res);
	case LI_COND_VALUE_NUMBER:
		return li_condition_check_eval_int(vr, cond, res);
	case LI_COND_VALUE_SOCKET_IPV4:
	case LI_COND_VALUE_SOCKET_IPV6:
		return li_condition_check_eval_ip(vr, cond, res);
	}
	VR_ERROR(vr, "Unsupported conditional type: %i", cond->rvalue.type);
	return LI_HANDLER_ERROR;
}

#include <lighttpd/base.h>

#if 0
static const liConditionValueType cond_value_hints[] = {
	/* LI_COMP_REQUEST_LOCALIP: */ LI_COND_VALUE_HINT_SOCKADDR,
	/* LI_COMP_REQUEST_LOCALPORT: */ LI_COND_VALUE_HINT_NUMBER,
	/* LI_COMP_REQUEST_REMOTEIP: */ LI_COND_VALUE_HINT_SOCKADDR,
	/* LI_COMP_REQUEST_REMOTEPORT: */ LI_COND_VALUE_HINT_NUMBER,
	/* LI_COMP_REQUEST_PATH: */ LI_COND_VALUE_HINT_STRING,
	/* LI_COMP_REQUEST_HOST: */ LI_COND_VALUE_HINT_ANY,
	/* LI_COMP_REQUEST_SCHEME: */ LI_COND_VALUE_HINT_STRING,
	/* LI_COMP_REQUEST_QUERY_STRING: */ LI_COND_VALUE_HINT_ANY,
	/* LI_COMP_REQUEST_METHOD: */ LI_COND_VALUE_HINT_STRING,
	/* LI_COMP_REQUEST_CONTENT_LENGTH: */ LI_COND_VALUE_HINT_NUMBER,
	/* LI_COMP_REQUEST_IS_HANDLED: */ LI_COND_VALUE_HINT_BOOL,
	/* LI_COMP_PHYSICAL_PATH: */ LI_COND_VALUE_HINT_STRING,
	/* LI_COMP_PHYSICAL_EXISTS: */ LI_COND_VALUE_HINT_BOOL,
	/* LI_COMP_PHYSICAL_SIZE: */ LI_COND_VALUE_HINT_NUMBER,
	/* LI_COMP_PHYSICAL_ISDIR: */ LI_COND_VALUE_HINT_BOOL,
	/* LI_COMP_PHYSICAL_ISFILE: */ LI_COND_VALUE_HINT_BOOL,
	/* LI_COMP_RESPONSE_STATUS: */ LI_COND_VALUE_HINT_NUMBER,
	/* LI_COMP_PHYSICAL_DOCROOT: */ LI_COND_VALUE_HINT_STRING,
	/* LI_COMP_PHYSICAL_PATHINFO: */ LI_COND_VALUE_HINT_STRING,

	/* LI_COMP_REQUEST_HEADER: */ LI_COND_VALUE_HINT_ANY,
	/* LI_COMP_RESPONSE_HEADER: */ LI_COND_VALUE_HINT_ANY,
	/* LI_COMP_ENVIRONMENT: */ LI_COND_VALUE_HINT_ANY,

	/* LI_COMP_UNKNOWN: */ LI_COND_VALUE_HINT_ANY
};
#endif

/* uses tmpstr for temporary (and returned) strings */
liHandlerResult li_condition_get_value(GString *tmpstr, liVRequest *vr, liConditionLValue *lvalue, liConditionValue *res, liConditionValueType prefer) {
	liConInfo *coninfo = vr->coninfo;
	liHandlerResult r;
	struct stat st;
	int err;

	res->match_type = LI_COND_VALUE_HINT_ANY;
	res->data.str = "";

	switch (lvalue->type) {
	case LI_COMP_REQUEST_LOCALIP:
		if (prefer == LI_COND_VALUE_HINT_STRING) {
			res->match_type = LI_COND_VALUE_HINT_STRING;
			res->data.str = coninfo->local_addr_str->str;
		} else {
			res->match_type = LI_COND_VALUE_HINT_SOCKADDR;
			res->data.addr = coninfo->local_addr;
		}
		break;
	case LI_COMP_REQUEST_LOCALPORT:
		res->match_type = LI_COND_VALUE_HINT_NUMBER;
		switch (coninfo->local_addr.addr->plain.sa_family) {
		case AF_INET:
			res->data.number = ntohs(coninfo->local_addr.addr->ipv4.sin_port);
			break;
		#ifdef HAVE_IPV6
		case AF_INET6:
			res->data.number = ntohs(coninfo->local_addr.addr->ipv6.sin6_port);
			break;
		#endif
		default:
			res->data.number = -1;
			break;
		}
		break;
	case LI_COMP_REQUEST_REMOTEIP:
		if (prefer == LI_COND_VALUE_HINT_STRING) {
			res->match_type = LI_COND_VALUE_HINT_STRING;
			res->data.str = coninfo->remote_addr_str->str;
		} else {
			res->match_type = LI_COND_VALUE_HINT_SOCKADDR;
			res->data.addr = coninfo->remote_addr;
		}
		break;
	case LI_COMP_REQUEST_REMOTEPORT:
		res->match_type = LI_COND_VALUE_HINT_NUMBER;
		switch (coninfo->remote_addr.addr->plain.sa_family) {
		case AF_INET:
			res->data.number = ntohs(coninfo->remote_addr.addr->ipv4.sin_port);
			break;
		#ifdef HAVE_IPV6
		case AF_INET6:
			res->data.number = ntohs(coninfo->remote_addr.addr->ipv6.sin6_port);
			break;
		#endif
		default:
			res->data.number = -1;
			break;
		}
		break;
	case LI_COMP_REQUEST_PATH:
		res->match_type = LI_COND_VALUE_HINT_STRING;
		res->data.str = vr->request.uri.path->str;
		break;
	case LI_COMP_REQUEST_HOST:
		res->match_type = LI_COND_VALUE_HINT_STRING;
		res->data.str = vr->request.uri.host->str;
		break;
	case LI_COMP_REQUEST_SCHEME:
		res->match_type = LI_COND_VALUE_HINT_STRING;
		res->data.str = coninfo->is_ssl ? "https" : "http";
		break;
	case LI_COMP_REQUEST_QUERY_STRING:
		res->data.str = vr->request.uri.query->str;
		break;
	case LI_COMP_REQUEST_METHOD:
		res->match_type = LI_COND_VALUE_HINT_STRING;
		res->data.str = vr->request.http_method_str->str;
		break;
	case LI_COMP_REQUEST_CONTENT_LENGTH:
		res->match_type = LI_COND_VALUE_HINT_NUMBER;
		res->data.number = vr->request.content_length;
		break;
	case LI_COMP_REQUEST_IS_HANDLED:
		res->match_type = LI_COND_VALUE_HINT_BOOL;
		res->data.bool = li_vrequest_is_handled(vr);
		break;
	case LI_COMP_PHYSICAL_PATH:
		res->match_type = LI_COND_VALUE_HINT_STRING;
		res->data.str = vr->physical.path->str;
		break;
	case LI_COMP_PHYSICAL_EXISTS:
	case LI_COMP_PHYSICAL_ISDIR:
	case LI_COMP_PHYSICAL_ISFILE:
		res->match_type = LI_COND_VALUE_HINT_BOOL;
		res->data.bool = FALSE;
		if (vr->physical.path->len == 0) {
			/* no file, return FALSE */
			break;
		}

		r = li_stat_cache_get(vr, vr->physical.path, &st, &err, NULL);
		if (r == LI_HANDLER_WAIT_FOR_EVENT) return r;

		/* not found, return FALSE */
		if (r != LI_HANDLER_GO_ON) break;
		if (lvalue->type == LI_COMP_PHYSICAL_ISFILE) {
			res->data.bool = S_ISREG(st.st_mode);
		} else if (lvalue->type == LI_COMP_PHYSICAL_ISDIR) {
			res->data.bool = S_ISDIR(st.st_mode);
		} else {
			res->data.bool = TRUE;
		}
		break;
	case LI_COMP_PHYSICAL_SIZE:
		res->match_type = LI_COND_VALUE_HINT_NUMBER;
		res->data.number = -1;
		if (vr->physical.path->len == 0) {
			break;
		}

		r = li_stat_cache_get(vr, vr->physical.path, &st, &err, NULL);
		if (r == LI_HANDLER_WAIT_FOR_EVENT) return r;

		if (r == LI_HANDLER_GO_ON) { /* not found -> size "-1" */
			res->data.number = (gint64) st.st_size;
		}
		break;
	case LI_COMP_PHYSICAL_DOCROOT:
		res->match_type = LI_COND_VALUE_HINT_STRING;
		res->data.str = vr->physical.doc_root->str;
		break;
	case LI_COMP_PHYSICAL_PATHINFO:
		res->match_type = LI_COND_VALUE_HINT_STRING;
		res->data.str = vr->physical.pathinfo->str;
		break;
	case LI_COMP_RESPONSE_STATUS:
		LI_VREQUEST_WAIT_FOR_RESPONSE_HEADERS(vr);
		res->match_type = LI_COND_VALUE_HINT_NUMBER;
		res->data.number = vr->response.http_status;
		break;
	case LI_COMP_REQUEST_HEADER:
		res->match_type = LI_COND_VALUE_HINT_STRING;
		li_http_header_get_all(tmpstr, vr->request.headers, GSTR_LEN(lvalue->key));
		res->data.str = tmpstr->str;
		break;
	case LI_COMP_RESPONSE_HEADER:
		LI_VREQUEST_WAIT_FOR_RESPONSE_HEADERS(vr);
		res->match_type = LI_COND_VALUE_HINT_STRING;
		li_http_header_get_all(tmpstr, vr->response.headers, GSTR_LEN(lvalue->key));
		res->data.str = tmpstr->str;
		break;
	case LI_COMP_ENVIRONMENT:
		res->match_type = LI_COND_VALUE_HINT_STRING;
		{
			GString *eval = li_environment_get(&vr->env, GSTR_LEN(lvalue->key));
			if (eval) res->data.str = eval->str;
		}
		break;
	default:
		VR_ERROR(vr, "couldn't get value for '%s'", li_cond_lvalue_to_string(lvalue->type));
		return LI_HANDLER_ERROR;
	}

	return LI_HANDLER_GO_ON;
}

gchar const* li_condition_value_to_string(GString *tmpstr, liConditionValue *value) {
	switch (value->match_type) {
	case LI_COND_VALUE_HINT_ANY:
	case LI_COND_VALUE_HINT_STRING:
		return value->data.str;
	case LI_COND_VALUE_HINT_BOOL:
		return value->data.bool ? "TRUE" : "FALSE";
	case LI_COND_VALUE_HINT_NUMBER:
		g_string_printf(tmpstr, "%"LI_GOFFSET_FORMAT, value->data.number);
		return tmpstr->str;
	case LI_COND_VALUE_HINT_SOCKADDR:
		li_sockaddr_to_string(value->data.addr, tmpstr, TRUE);
		return tmpstr->str;
	}
	return "";
}

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
	if (type == LI_COMP_REQUEST_HEADER || type == LI_COMP_RESPONSE_HEADER) g_string_ascii_down(key);
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
	case LI_COMP_REQUEST_LOCALPORT: return "request.localport";
	case LI_COMP_REQUEST_REMOTEIP: return "request.remoteip";
	case LI_COMP_REQUEST_REMOTEPORT: return "request.remoteport";
	case LI_COMP_REQUEST_PATH: return "request.path";
	case LI_COMP_REQUEST_HOST: return "request.host";
	case LI_COMP_REQUEST_SCHEME: return "request.scheme";
	case LI_COMP_REQUEST_QUERY_STRING: return "request.query";
	case LI_COMP_REQUEST_METHOD: return "request.method";
	case LI_COMP_REQUEST_CONTENT_LENGTH: return "request.length";
	case LI_COMP_REQUEST_IS_HANDLED: return "request.is_handled";
	case LI_COMP_PHYSICAL_PATH: return "physical.path";
	case LI_COMP_PHYSICAL_EXISTS: return "physical.exist";
	case LI_COMP_PHYSICAL_SIZE: return "physical.size";
	case LI_COMP_PHYSICAL_ISDIR: return "physical.is_dir";
	case LI_COMP_PHYSICAL_ISFILE: return "physical.is_file";
	case LI_COMP_PHYSICAL_DOCROOT: return "physical.docroot";
	case LI_COMP_PHYSICAL_PATHINFO: return "physical.pathinfo";
	case LI_COMP_RESPONSE_STATUS: return "response.status";
	case LI_COMP_REQUEST_HEADER: return "request.header";
	case LI_COMP_RESPONSE_HEADER: return "response.header";
	case LI_COMP_ENVIRONMENT: return "request.environment";
	case LI_COMP_UNKNOWN: return "<unknown>";
	}

	return "<unkown>";
}

liCondLValue li_cond_lvalue_from_string(const gchar *str, guint len) {
	gchar *c = (gchar*)str;

	if (g_str_has_prefix(c, "req")) {
		if (g_str_has_prefix(c, "req.")) {
			c += sizeof("req.")-1;
			len -= sizeof("req.")-1;
		} else if (g_str_has_prefix(c, "request.")) {
			c += sizeof("request.")-1;
			len -= sizeof("request.")-1;
		} else {
			return LI_COMP_UNKNOWN;
		}

		if (strncmp(c, "localip", len) == 0)
			return LI_COMP_REQUEST_LOCALIP;
		else if (strncmp(c, "localport", len) == 0)
			return LI_COMP_REQUEST_LOCALPORT;
		else if (strncmp(c, "remoteip", len) == 0)
			return LI_COMP_REQUEST_REMOTEIP;
		else if (strncmp(c, "remoteport", len) == 0)
			return LI_COMP_REQUEST_REMOTEPORT;
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
		else if (strncmp(c, "environment", len) == 0 || strncmp(c, "env", len) == 0)
			return LI_COMP_ENVIRONMENT;
		else if (strncmp(c, "is_handled", len) == 0)
			return LI_COMP_REQUEST_IS_HANDLED;
	} else if (g_str_has_prefix(c, "phys")) {
		if (g_str_has_prefix(c, "phys.")) {
			c += sizeof("phys.")-1;
			len -= sizeof("phys.")-1;
		} else if (g_str_has_prefix(c, "physical.")) {
			c += sizeof("physical.")-1;
			len -= sizeof("physical.")-1;
		} else {
			return LI_COMP_UNKNOWN;
		}

		if (strncmp(c, "path", len) == 0)
			return LI_COMP_PHYSICAL_PATH;
		else if (strncmp(c, "exists", len) == 0)
			return LI_COMP_PHYSICAL_EXISTS;
		else if (strncmp(c, "size", len) == 0)
			return LI_COMP_PHYSICAL_SIZE;
		else if (strncmp(c, "is_file", len) == 0)
			return LI_COMP_PHYSICAL_ISFILE;
		else if (strncmp(c, "is_dir", len) == 0)
			return LI_COMP_PHYSICAL_ISDIR;
		else if (strncmp(c, "docroot", len) == 0)
			return LI_COMP_PHYSICAL_DOCROOT;
		else if (strncmp(c, "pathinfo", len) == 0)
			return LI_COMP_PHYSICAL_PATHINFO;
	} else if (g_str_has_prefix(c, "resp")) {
		if (g_str_has_prefix(c, "resp.")) {
			c += sizeof("resp.")-1;
			len -= sizeof("resp.")-1;
		} else if (g_str_has_prefix(c, "response.")) {
			c += sizeof("response.")-1;
			len -= sizeof("response.")-1;
		} else {
			return LI_COMP_UNKNOWN;
		}

		if (strncmp(c, "status", len) == 0)
			return LI_COMP_RESPONSE_STATUS;
		else if (strncmp(c, "header", len) == 0)
			return LI_COMP_RESPONSE_HEADER;
	}

	return LI_COMP_UNKNOWN;
}

static liHandlerResult li_condition_check_eval_bool(liVRequest *vr, liCondition *cond, gboolean *res) {
	gboolean val;
	liConditionValue match_val;
	liHandlerResult r;
	*res = FALSE;

	r = li_condition_get_value(vr->wrk->tmp_str, vr, cond->lvalue, &match_val, LI_COND_VALUE_HINT_BOOL);
	if (r != LI_HANDLER_GO_ON) return r;

	switch (match_val.match_type) {
	case LI_COND_VALUE_HINT_ANY:
	case LI_COND_VALUE_HINT_STRING:
		val = ('\0' != match_val.data.str[0]);
		break;
	case LI_COND_VALUE_HINT_BOOL:
		val = match_val.data.bool;
		break;
	case LI_COND_VALUE_HINT_NUMBER:
		val = match_val.data.number != 0;
		break;
	case LI_COND_VALUE_HINT_SOCKADDR:
		val = TRUE; /* just.. don't do this. */
		break;
	default:
		return LI_HANDLER_ERROR;
	}

	*res = !cond->rvalue.b ^ val;

	return LI_HANDLER_GO_ON;
}

/* LI_COND_VALUE_STRING and LI_COND_VALUE_REGEXP only */
static liHandlerResult li_condition_check_eval_string(liVRequest *vr, liCondition *cond, gboolean *res) {
	liActionRegexStackElement arse;
	liConditionValue match_val;
	liHandlerResult r;
	const char *val = "";
	*res = FALSE;

	r = li_condition_get_value(vr->wrk->tmp_str, vr, cond->lvalue, &match_val, LI_COND_VALUE_HINT_STRING);
	if (r != LI_HANDLER_GO_ON) return r;

	val = li_condition_value_to_string(vr->wrk->tmp_str, &match_val);

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
		arse.match_info = NULL;
		arse.string = g_string_new(val); /* we have to copy the value, as match-info references it */
		*res = g_regex_match(cond->rvalue.regex, arse.string->str, 0, &arse.match_info);
		if (*res) {
			g_array_append_val(vr->action_stack.regex_stack, arse);
		} else {
			g_match_info_free(arse.match_info);
			g_string_free(arse.string, TRUE);
		}
		break;
	case LI_CONFIG_COND_NOMATCH:
		arse.match_info = NULL;
		arse.string = g_string_new(val); /* we have to copy the value, as match-info references it */
		*res = !g_regex_match(cond->rvalue.regex, arse.string->str, 0, &arse.match_info);
		if (*res) {
			g_match_info_free(arse.match_info);
			g_string_free(arse.string, TRUE);
		} else {
			g_array_append_val(vr->action_stack.regex_stack, arse);
		}
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
	liConditionValue match_val;
	liHandlerResult r;
	*res = FALSE;

	r = li_condition_get_value(vr->wrk->tmp_str, vr, cond->lvalue, &match_val, LI_COND_VALUE_HINT_NUMBER);
	if (r != LI_HANDLER_GO_ON) return r;

	switch (match_val.match_type) {
	case LI_COND_VALUE_HINT_ANY:
		val = g_ascii_strtoll(match_val.data.str, NULL, 10);
		errno = 0; /* ignore errors */
		break;
	case LI_COND_VALUE_HINT_NUMBER:
		val = match_val.data.number;
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

static gboolean ip_in_net(liConditionRValue *target, liConditionRValue *network) {
	if (target->type == LI_COND_VALUE_SOCKET_IPV4) {
		if (network->type == LI_COND_VALUE_SOCKET_IPV4) {
			return li_ipv4_in_ipv4_net(target->ipv4.addr, network->ipv4.addr, network->ipv4.networkmask);
		} else if (network->type == LI_COND_VALUE_SOCKET_IPV6) {
			/* strict matches only */
			/* return li_ipv4_in_ipv6_net(target->ipv4.addr, network->ipv6.addr, network->ipv6.network); */
			return FALSE;
		}
	} else if (target->type == LI_COND_VALUE_SOCKET_IPV6) {
		if (network->type == LI_COND_VALUE_SOCKET_IPV4) {
			/* strict matches only */
			/* return li_ipv6_in_ipv4_net(target->ipv6.addr, network->ipv4.addr, network->ipv4.networkmask); */
			return FALSE;
		} else if (network->type == LI_COND_VALUE_SOCKET_IPV6) {
			return li_ipv6_in_ipv6_net(target->ipv6.addr, network->ipv6.addr, network->ipv6.network);
		}
	}
	return FALSE;
}

/* LI_CONFIG_COND_IP and LI_CONFIG_COND_NOTIP only */
static liHandlerResult li_condition_check_eval_ip(liVRequest *vr, liCondition *cond, gboolean *res) {
	liConditionValue match_val;
	liHandlerResult r;
	liConditionRValue ipval;

	r = li_condition_get_value(vr->wrk->tmp_str, vr, cond->lvalue, &match_val, LI_COND_VALUE_HINT_SOCKADDR);
	if (r != LI_HANDLER_GO_ON) return r;

	*res = (cond->op == LI_CONFIG_COND_NOTIP);

	switch (match_val.match_type) {
	case LI_COND_VALUE_HINT_ANY:
		if (!condition_parse_ip(&ipval, match_val.data.str))
			return LI_HANDLER_GO_ON;
		break;
	case LI_COND_VALUE_HINT_SOCKADDR:
		if (!condition_ip_from_socket(&ipval, match_val.data.addr.addr))
			return LI_HANDLER_GO_ON;
		break;
	default:
		VR_ERROR(vr, "couldn't get int value for '%s'", li_cond_lvalue_to_string(cond->lvalue->type));
		return LI_HANDLER_ERROR;
	}

	switch (cond->op) {
	case LI_CONFIG_COND_IP:
		*res = ip_in_net(&ipval, &cond->rvalue);
		break;
	case LI_CONFIG_COND_NOTIP:
		*res = !ip_in_net(&ipval, &cond->rvalue);
		break;
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

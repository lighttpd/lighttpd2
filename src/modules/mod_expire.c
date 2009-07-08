/*
 * mod_expire - add "Expires" and "Cache-Control" headers to response
 *
 * Description:
 *     mod_expire lets you control client-side caching of responses based on a simple rule/formula.
 *     If a response is cached using an "Expires" and "Cache-Control" header, then the client will not issue a new
 *     request for it until the date specified by the header is reached.
 *
 *     The rule/formula used here, complies with the one mod_expire for Apache uses:
 *     <base> [plus] (<num> <type>)+
 *     Where <base> is one of "access", "now" or "modification" - "now" being equivalent to "access".
 *     <plus> is optional and does nothing.
 *     <num> is any positive integer.
 *     <type> is one of "seconds, "minutes", "hours", "days", "weeks", "months" or "years".
 *     The trailing s in <type> is optional.
 *
 *     If you use "modification" as <base> and the file does not exist or cannot be accessed,
 *     mod_expire will do nothing and request processing will go on.
 *
 *     The expire action will overwrite any existing "Expires" header.
 *     It will append the max-age value to any existing "Cache-Control" header.
 *
 * Setups:
 *     none
 * Options:
 *     none
 * Actions:
 *     expire "rule";
 *         - adds an Expires header to the response based on the specified rule.
 *
 * Example config:
 *     if request.path =~ "^/(css|js|images)/" {
 *         expire "access plus 1 day";
 *     }
 *
 *
 * Tip:
 *     Adding expire headers to static content like css, javascript, images or similar,
 *     can greatly reduce the amount of requests you get and therefor save resources.
 *     Use "modification" as <base> if your content changes in specific intervals like every 15 minutes.
 *
 * Todo:
 *     none
 *
 * Author:
 *     Copyright (c) 2009 Thomas Porzelt
 * License:
 *     MIT, see COPYING file in the lighttpd 2 tree
 */

#include <lighttpd/base.h>

LI_API gboolean mod_expire_init(liModules *mods, liModule *mod);
LI_API gboolean mod_expire_free(liModules *mods, liModule *mod);


struct expire_rule {
	enum {
		EXPIRE_ACCESS,
		EXPIRE_MODIFICATION
	} base;
	guint num;
};
typedef struct expire_rule expire_rule;


static liHandlerResult expire(liVRequest *vr, gpointer param, gpointer *context) {
	struct tm tm;
	time_t expire_date;
	guint len;
	gint max_age;
	GString *date_str = vr->wrk->tmp_str;
	expire_rule *rule = param;
	guint num = rule->num;
	time_t now = (time_t)CUR_TS(vr->wrk);

	UNUSED(context);

	if (rule->base == EXPIRE_ACCESS) {
		expire_date = now + num;
		max_age = num;
	} else {
		/* modification */
		struct stat st;
		gint err;

		if (!vr->physical.path->len)
			return LI_HANDLER_GO_ON;

		switch (stat_cache_get(vr, vr->physical.path, &st, &err, NULL)) {
		case LI_HANDLER_GO_ON: break;
		case LI_HANDLER_WAIT_FOR_EVENT: return LI_HANDLER_WAIT_FOR_EVENT;
		default: return LI_HANDLER_GO_ON;
		}

		expire_date = st.st_mtime + num;

		if (expire_date < now)
			expire_date = now;

		max_age = expire_date - now;
	}

	/* format date */
	g_string_set_size(date_str, 255);

	if (!gmtime_r(&expire_date, &tm)) {
		VR_ERROR(vr, "gmtime_r(%"G_GUINT64_FORMAT") failed: %s", (guint64)expire_date, g_strerror(errno));
		return LI_HANDLER_GO_ON;
	}

	len = strftime(date_str->str, date_str->allocated_len, "%a, %d %b %Y %H:%M:%S GMT", &tm);
	if (len == 0)
		return LI_HANDLER_GO_ON;

	g_string_set_size(date_str, len);

	/* finally set the headers */
	http_header_overwrite(vr->response.headers, CONST_STR_LEN("Expires"), GSTR_LEN(date_str));
	g_string_truncate(date_str, 0);
	g_string_append_len(date_str, CONST_STR_LEN("max-age="));
	l_g_string_append_int(date_str, max_age);
	http_header_append(vr->response.headers, CONST_STR_LEN("Cache-Control"), GSTR_LEN(date_str));

	return LI_HANDLER_GO_ON;
}

static void expire_free(liServer *srv, gpointer param) {
	UNUSED(srv);

	g_slice_free(expire_rule, param);
}

static liAction* expire_create(liServer *srv, liPlugin* p, liValue *val) {
	expire_rule *rule;
	gchar *str;

	UNUSED(srv);
	UNUSED(p);

	if (!val || val->type != LI_VALUE_STRING) {
		ERROR(srv, "%s", "expire expects a string as parameter");
		return NULL;
	}

	rule = g_slice_new(expire_rule);

	str = val->data.string->str;

	/* check if we have "access", "now" or "modification as <base> */
	if (g_str_has_prefix(str, "access ")) {
		rule->base = EXPIRE_ACCESS;
		str += sizeof("access ") - 1;
	} else if (g_str_has_prefix(str, "now ")) {
		rule->base = EXPIRE_ACCESS;
		str += sizeof("now ") - 1;
	} else if (g_str_has_prefix(str, "modification ")) {
		rule->base = EXPIRE_MODIFICATION;
		str += sizeof("modification ") - 1;
	} else {
		g_slice_free(expire_rule, rule);
		ERROR(srv, "expire: error parsing rule \"%s\"", val->data.string->str);
		return NULL;
	}

	/* skip the optional "plus", it does nothing */
	if (g_str_has_prefix(str, "plus "))
		str += sizeof("plus ") - 1;

	/* parse <num> */
	rule->num = 0;

	for (; *str; str++) {
		if (*str < '0' || *str > '9')
			break;

		rule->num += (*str) - '0';
	}

	if (!rule->num) {
		g_slice_free(expire_rule, rule);
		ERROR(srv, "expire: error parsing rule \"%s\", <num> must be a positive integer", val->data.string->str);
		return NULL;
	}

	/* parse <type> */
	if (g_str_equal(str, " second") || g_str_equal(str, " seconds"))
		rule->num *= 1;
	else if (g_str_equal(str, " minute") || g_str_equal(str, " minutes"))
		rule->num *= 60;
	else if (g_str_equal(str, " hour") || g_str_equal(str, " hours"))
		rule->num *= 3600;
	else if (g_str_equal(str, " day") || g_str_equal(str, " days"))
		rule->num *= 3600*24;
	else if (g_str_equal(str, " week") || g_str_equal(str, " weeks"))
		rule->num *= 3600*24*7;
	else if (g_str_equal(str, " month") || g_str_equal(str, " months"))
		rule->num *= 3600*24*30;
	else if (g_str_equal(str, " year") || g_str_equal(str, " years"))
		rule->num *= 3600*24*365;
	else {
		g_slice_free(expire_rule, rule);
		ERROR(srv, "expire: error parsing rule \"%s\", <type> must be one of 'seconds', 'minutes', 'hours', 'days', 'weeks', 'months' or 'years'", val->data.string->str);
		return NULL;
	}

	return action_new_function(expire, NULL, expire_free, rule);
}



static const liPluginOption options[] = {
	{ NULL, 0, NULL, NULL, NULL }
};

static const liPluginAction actions[] = {
	{ "expire", expire_create },

	{ NULL, NULL }
};

static const liliPluginSetupCB setups[] = {
	{ NULL, NULL }
};


static void plugin_expire_init(liServer *srv, liPlugin *p) {
	UNUSED(srv);

	p->options = options;
	p->actions = actions;
	p->setups = setups;
}


gboolean mod_expire_init(liModules *mods, liModule *mod) {
	UNUSED(mod);

	MODULE_VERSION_CHECK(mods);

	mod->config = plugin_register(mods->main, "mod_expire", plugin_expire_init);

	return mod->config != NULL;
}

gboolean mod_expire_free(liModules *mods, liModule *mod) {
	if (mod->config)
		plugin_free(mods->main, mod->config);

	return TRUE;
}

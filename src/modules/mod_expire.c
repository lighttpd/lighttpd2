/*
 * mod_expire - add "Expires" and "Cache-Control" headers to response
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
	time_t now = (time_t)li_cur_ts(vr->wrk);

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

		switch (li_stat_cache_get(vr, vr->physical.path, &st, &err, NULL)) {
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
	li_http_header_overwrite(vr->response.headers, CONST_STR_LEN("Expires"), GSTR_LEN(date_str));
	g_string_truncate(date_str, 0);
	g_string_append_len(date_str, CONST_STR_LEN("max-age="));
	li_string_append_int(date_str, max_age);
	li_http_header_append(vr->response.headers, CONST_STR_LEN("Cache-Control"), GSTR_LEN(date_str));

	return LI_HANDLER_GO_ON;
}

static void expire_free(liServer *srv, gpointer param) {
	UNUSED(srv);

	g_slice_free(expire_rule, param);
}

static liAction* expire_create(liServer *srv, liWorker *wrk, liPlugin* p, liValue *val, gpointer userdata) {
	expire_rule *rule;
	gchar *str;
	UNUSED(wrk); UNUSED(p); UNUSED(userdata);

	val = li_value_get_single_argument(val);

	if (LI_VALUE_STRING != li_value_type(val)) {
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

	rule->num = 0;

	/* parse (<num> <type>)+ */
	while (*str) {
		guint num;
		/* parse <num> */
		num = 0;

		for (; *str; str++) {
			if (*str < '0' || *str > '9')
				break;

			num *= 10;
			num += (*str) - '0';
		}

		if (!num) {
			g_slice_free(expire_rule, rule);
			ERROR(srv, "expire: error parsing rule \"%s\", <num> must be a positive integer", val->data.string->str);
			return NULL;
		}

		/* parse <type> */
		if (g_str_has_prefix(str, " second")) {
			num *= 1;
			str += sizeof(" second") - 1;
		} else if (g_str_has_prefix(str, " minute")) {
			num *= 60;
			str += sizeof(" minute") - 1;
		} else if (g_str_has_prefix(str, " hour")) {
			num *= 3600;
			str += sizeof(" hour") - 1;
		} else if (g_str_has_prefix(str, " day")) {
			num *= 3600*24;
			str += sizeof(" day") - 1;
		} else if (g_str_has_prefix(str, " week")) {
			num *= 3600*24*7;
			str += sizeof(" week") - 1;
		} else if (g_str_has_prefix(str, " month")) {
			num *= 3600*24*30;
			str += sizeof(" month") - 1;
		} else if (g_str_has_prefix(str, " year")) {
			num *= 3600*24*365;
			str += sizeof(" year") - 1;
		} else {
			g_slice_free(expire_rule, rule);
			ERROR(srv, "expire: error parsing rule \"%s\", <type> must be one of 'seconds', 'minutes', 'hours', 'days', 'weeks', 'months' or 'years'", val->data.string->str);
			return NULL;
		}

		rule->num += num;

		if (*str == 's')
			str++;

		if (*str == ' ')
			str++;
		else if (*str) {
			g_slice_free(expire_rule, rule);
			ERROR(srv, "expire: error parsing rule \"%s\", <type> must be one of 'seconds', 'minutes', 'hours', 'days', 'weeks', 'months' or 'years'", val->data.string->str);
			return NULL;
		}
	}

	return li_action_new_function(expire, NULL, expire_free, rule);
}



static const liPluginOption options[] = {
	{ NULL, 0, 0, NULL }
};

static const liPluginAction actions[] = {
	{ "expire", expire_create, NULL },

	{ NULL, NULL, NULL }
};

static const liPluginSetup setups[] = {
	{ NULL, NULL, NULL }
};


static void plugin_expire_init(liServer *srv, liPlugin *p, gpointer userdata) {
	UNUSED(srv); UNUSED(userdata);

	p->options = options;
	p->actions = actions;
	p->setups = setups;
}


gboolean mod_expire_init(liModules *mods, liModule *mod) {
	UNUSED(mod);

	MODULE_VERSION_CHECK(mods);

	mod->config = li_plugin_register(mods->main, "mod_expire", plugin_expire_init, NULL);

	return mod->config != NULL;
}

gboolean mod_expire_free(liModules *mods, liModule *mod) {
	if (mod->config)
		li_plugin_free(mods->main, mod->config);

	return TRUE;
}

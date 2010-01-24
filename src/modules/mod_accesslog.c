/*
 * mod_accesslog - log access to the server
 *
 * Description:
 *     mod_accesslog can log requests handled by lighttpd to files, pipes or syslog
 *     the format of the logs can be customized by using printf-style placeholders
 *
 * Setups:
 *     none
 * Options:
 *     accesslog = <file>;           - log target
 *         type: string
 *         default: none
 *     accesslog.format = <format>;  - log format
 *         type: string
 *         default: "%h %V %u %t \"%r\" %>s %b \"%{Referer}i\" \"%{User-Agent}i\""
 * Actions:
 *     none
 *
 * Example config:
 *     accesslog = "/var/log/lighttpd/access.log";
 *     accesslog.format = "%h %V %u %t \"%r\" %>s %b \"%{Referer}i\" \"%{User-Agent}i\"";
 *
 * Todo:
 *     - implement format key for %t: %{format}t
 *     - implement missing format identifiers
 *
 * Author:
 *     Copyright (c) 2009 Thomas Porzelt
 * License:
 *     MIT, see COPYING file in the lighttpd 2 tree
 */

#include <lighttpd/base.h>
#include <lighttpd/plugin_core.h>

LI_API gboolean mod_accesslog_init(liModules *mods, liModule *mod);
LI_API gboolean mod_accesslog_free(liModules *mods, liModule *mod);

struct al_data {
	guint ts_ndx;
};
typedef struct al_data al_data;

enum {
	AL_OPTION_ACCESSLOG = 0,
	AL_OPTION_ACCESSLOG_FORMAT
};

typedef struct {
	gchar character;
	gboolean need_key;
	enum {
		AL_FORMAT_UNSUPPORTED,
		AL_FORMAT_PERCENT,
		AL_FORMAT_REMOTE_ADDR,
		AL_FORMAT_LOCAL_ADDR,
		AL_FORMAT_BYTES_RESPONSE,        /* without headers */
		AL_FORMAT_BYTES_RESPONSE_CLF,    /* same as above but - instead of 0 */
		AL_FORMAT_COOKIE,
		AL_FORMAT_DURATION_MICROSECONDS, /* duration of request in microseconds */
		AL_FORMAT_ENV,                   /* environment var */
		AL_FORMAT_FILENAME,
		AL_FORMAT_REMOTE_HOST,
		AL_FORMAT_PROTOCOL,
		AL_FORMAT_REQUEST_HEADER,
		AL_FORMAT_METHOD,
		AL_FORMAT_RESPONSE_HEADER,
		AL_FORMAT_LOCAL_PORT,
		AL_FORMAT_QUERY_STRING,
		AL_FORMAT_FIRST_LINE,            /* GET /foo?bar HTTP/1.1 */
		AL_FORMAT_STATUS_CODE,
		AL_FORMAT_TIME,                  /* standard english format */
		AL_FORMAT_DURATION_SECONDS,
		AL_FORMAT_AUTHED_USER,
		AL_FORMAT_PATH,
		AL_FORMAT_SERVER_NAME,
		AL_FORMAT_HOSTNAME,
		AL_FORMAT_CONNECTION_STATUS,     /* X = not complete, + = keep alive, - = no keep alive */
		AL_FORMAT_BYTES_IN,
		AL_FORMAT_BYTES_OUT
	} type;
} al_format;

typedef struct {
	al_format format;
	GString *key;
	enum { AL_ENTRY_FORMAT, AL_ENTRY_STRING } type;
} al_format_entry;

static const al_format al_format_mapping[] = {
	{ '%', FALSE, AL_FORMAT_PERCENT },
	{ 'a', FALSE, AL_FORMAT_REMOTE_ADDR },
	{ 'A', FALSE, AL_FORMAT_LOCAL_ADDR },
	{ 'b', FALSE, AL_FORMAT_BYTES_RESPONSE },
	{ 'B', FALSE, AL_FORMAT_BYTES_RESPONSE_CLF },
	{ 'C', FALSE, AL_FORMAT_COOKIE },
	{ 'D', FALSE, AL_FORMAT_DURATION_MICROSECONDS },
	{ 'e', TRUE, AL_FORMAT_ENV },
	{ 'f', FALSE, AL_FORMAT_FILENAME },
	{ 'h', FALSE, AL_FORMAT_REMOTE_ADDR },
	{ 'i', TRUE, AL_FORMAT_REQUEST_HEADER },
	{ 'm', FALSE, AL_FORMAT_METHOD },
	{ 'o', TRUE, AL_FORMAT_RESPONSE_HEADER },
	{ 'p', FALSE, AL_FORMAT_LOCAL_PORT },
	{ 'q', FALSE, AL_FORMAT_QUERY_STRING },
	{ 'r', FALSE, AL_FORMAT_FIRST_LINE },
	{ 's', FALSE, AL_FORMAT_STATUS_CODE },
	{ 't', FALSE, AL_FORMAT_TIME },
	{ 'T', FALSE, AL_FORMAT_DURATION_SECONDS },
	{ 'u', FALSE, AL_FORMAT_AUTHED_USER },
	{ 'U', FALSE, AL_FORMAT_PATH },
	{ 'v', FALSE, AL_FORMAT_SERVER_NAME },
	{ 'V', FALSE, AL_FORMAT_HOSTNAME },
	{ 'X', FALSE, AL_FORMAT_CONNECTION_STATUS },
	{ 'I', FALSE, AL_FORMAT_BYTES_IN },
	{ 'O', FALSE, AL_FORMAT_BYTES_OUT },

	{ '\0', FALSE, AL_FORMAT_UNSUPPORTED }
};



static void al_append_escaped(GString *log, GString *str) {
	/* replaces non-printable chars with \xHH where HH is the hex representation of the byte */
	/* exceptions: " => \", \ => \\, whitespace chars => \n \t etc. */
	for (guint i = 0; i < str->len; i++) {
		if (str->str[i] >= ' ' && str->str[i] <= '~') {
			/* printable chars */
			g_string_append_c(log, str->str[i]);
		} else {
			switch (str->str[i]) {
			case '"': g_string_append_len(log, CONST_STR_LEN("\\\"")); break;
			case '\\': g_string_append_len(log, CONST_STR_LEN("\\\\")); break;
			case '\b': g_string_append_len(log, CONST_STR_LEN("\\b")); break;
			case '\n': g_string_append_len(log, CONST_STR_LEN("\\n")); break;
			case '\r': g_string_append_len(log, CONST_STR_LEN("\\r")); break;
			case '\t': g_string_append_len(log, CONST_STR_LEN("\\t")); break;
			case '\v': g_string_append_len(log, CONST_STR_LEN("\\v")); break;
			default:
				{
					/* non printable char => \xHH */
					gchar hh[5] = {'\\','x',0,0,0};
					gchar h = str->str[i] / 16;
					hh[2] = (h > 9) ? (h - 10 + 'A') : (h + '0');
					h = str->str[i] % 16;
					hh[3] = (h > 9) ? (h - 10 + 'A') : (h + '0');
					g_string_append_len(log, &hh[0], 4);
				}
				break;
			}
		}
	}
}


static al_format al_get_format(gchar c) {
	guint i;
	for (i = 0; al_format_mapping[i].type != AL_FORMAT_UNSUPPORTED; i++) {
		if (al_format_mapping[i].character == c)
			break;
	}
	
	return al_format_mapping[i];
}


#define AL_PARSE_ERROR() \
	do { \
		if (e.key) \
			g_string_free(e.key, TRUE); \
		for (guint i = 0; i < arr->len; i++) \
			if (g_array_index(arr, al_format_entry, i).key) \
				g_string_free(g_array_index(arr, al_format_entry, i).key, TRUE); \
		g_array_free(arr, TRUE); \
		return NULL; \
	} while (0)

static GArray *al_parse_format(liServer *srv, GString *formatstr) {
	GArray *arr = g_array_new(FALSE, TRUE, sizeof(al_format_entry));
	al_format_entry e;
	gchar *c, *k;
	
	for (c = formatstr->str; *c != '\0';) {

		if (*c == '%') {
			c++;
			e.type = AL_ENTRY_FORMAT;
			e.key = NULL;
			if (*c == '\0')
				AL_PARSE_ERROR();
			if (*c == '<' || *c == '>')
				/* we ignore < and > */
				c++;
			if (*c == '{') {
				/* %{key} */
				c++;
				for (k = c; *k != '}'; k++) /* skip to next } */
					if (*k == '\0')
						AL_PARSE_ERROR();
				e.key = g_string_new_len(c, k - c);
				c = k+1;
			}
			e.format = al_get_format(*c);
			if (e.format.type == AL_FORMAT_UNSUPPORTED) {
				ERROR(srv, "unknown format identifier: %c", *c);
				AL_PARSE_ERROR();
			}
			if (!e.key && e.format.need_key) {
				ERROR(srv, "format identifier \"%c\" needs a key", e.format.character);
				AL_PARSE_ERROR();
			}
			c++;
		} else {
			/* normal string */
			e.type = AL_ENTRY_STRING;
			for (k = (c+1); *k != '\0' && *k != '%'; k++); /* skip to next % */
			e.key = g_string_new_len(c, k - c);
			c = k;
		}

		g_array_append_val(arr, e);
	}

	return arr;
}

static GString *al_format_log(liVRequest *vr, al_data *ald, GArray *format) {
	GString *str = g_string_sized_new(255);
	liResponse *resp = &vr->response;
	liRequest *req = &vr->request;
	liPhysical *phys = &vr->physical;
	gchar *tmp_str = NULL;
	GString *tmp_gstr = g_string_sized_new(127);
	GString *tmp_gstr2 = NULL;
	guint len = 0;

	for (guint i = 0; i < format->len; i++) {
		al_format_entry *e = &g_array_index(format, al_format_entry, i);
		if (e->type == AL_ENTRY_FORMAT) {
			switch (e->format.type) {
			case AL_FORMAT_PERCENT:
				g_string_append_c(str, '%');
				break;
			case AL_FORMAT_REMOTE_ADDR:
				g_string_append_len(str, GSTR_LEN(vr->con->remote_addr_str));
				break;
			case AL_FORMAT_LOCAL_ADDR:
				g_string_append_len(str, GSTR_LEN(vr->con->local_addr_str));
				break;
			case AL_FORMAT_BYTES_RESPONSE:
				li_string_append_int(str, vr->vr_out->bytes_out);
				break;
			case AL_FORMAT_BYTES_RESPONSE_CLF:
				if (vr->vr_out->bytes_out)
					li_string_append_int(str, vr->vr_out->bytes_out);
				else
					g_string_append_c(str, '-');
				break;
			case AL_FORMAT_DURATION_MICROSECONDS:
				li_string_append_int(str, (CUR_TS(vr->wrk) - vr->ts_started) * 1000 * 1000);
				break;
			case AL_FORMAT_ENV:
				tmp_gstr2 = li_environment_get(&vr->env, GSTR_LEN(e->key));
				if (tmp_gstr2)
					al_append_escaped(str, tmp_gstr);
				else
					g_string_append_c(str, '-');
				break;
			case AL_FORMAT_FILENAME:
				if (phys->path->len)
					g_string_append_len(str, GSTR_LEN(phys->path));
				else
					g_string_append_c(str, '-');
				break;
			case AL_FORMAT_REQUEST_HEADER:
				li_http_header_get_all(tmp_gstr, req->headers, GSTR_LEN(e->key));
				if (tmp_gstr->len)
					al_append_escaped(str, tmp_gstr);
				else
					g_string_append_c(str, '-');
				break;
			case AL_FORMAT_METHOD:
				g_string_append_len(str, GSTR_LEN(req->http_method_str));
				break;
			case AL_FORMAT_RESPONSE_HEADER:
				li_http_header_get_all(tmp_gstr, resp->headers, GSTR_LEN(e->key));
				if (tmp_gstr->len)
					al_append_escaped(str, tmp_gstr);
				else
					g_string_append_c(str, '-');
				break;
			case AL_FORMAT_LOCAL_PORT:
				switch (vr->con->local_addr.addr->plain.sa_family) {
				case AF_INET: li_string_append_int(str, ntohs(vr->con->local_addr.addr->ipv4.sin_port)); break;
				#ifdef HAVE_IPV6
				case AF_INET6: li_string_append_int(str, ntohs(vr->con->local_addr.addr->ipv6.sin6_port)); break;
				#endif
				default: g_string_append_c(str, '-'); break;
				}
				break;
			case AL_FORMAT_QUERY_STRING:
				if (req->uri.query->len)
					al_append_escaped(str, req->uri.query);
				else
					g_string_append_c(str, '-');
				break;
			case AL_FORMAT_FIRST_LINE:
				g_string_append_len(str, GSTR_LEN(req->http_method_str));
				g_string_append_c(str, ' ');
				al_append_escaped(str, req->uri.raw_orig_path);
				if (req->uri.query->len) {
					g_string_append_c(str, '?');
					al_append_escaped(str, req->uri.query);
				}
				g_string_append_c(str, ' ');
				tmp_str = li_http_version_string(req->http_version, &len);
				g_string_append_len(str, tmp_str, len);
				break;
			case AL_FORMAT_STATUS_CODE:
				li_string_append_int(str, resp->http_status);
				break;
			case AL_FORMAT_TIME:
				/* todo: implement format string */
				tmp_gstr2 = li_worker_current_timestamp(vr->wrk, LI_LOCALTIME, ald->ts_ndx);
				g_string_append_len(str, GSTR_LEN(tmp_gstr2));
				break;
			case AL_FORMAT_DURATION_SECONDS:
				li_string_append_int(str, CUR_TS(vr->wrk) - vr->ts_started);
				break;
			case AL_FORMAT_AUTHED_USER:
				tmp_gstr2 = li_environment_get(&vr->env, CONST_STR_LEN("REMOTE_USER"));
				if (tmp_gstr2)
					g_string_append_len(str, GSTR_LEN(tmp_gstr));
				else
					g_string_append_c(str, '-');
				break;
			case AL_FORMAT_PATH:
				g_string_append_len(str, GSTR_LEN(req->uri.path));
				break;
			case AL_FORMAT_SERVER_NAME:
				if (CORE_OPTIONPTR(LI_CORE_OPTION_SERVER_NAME).string)
					g_string_append_len(str, GSTR_LEN(CORE_OPTIONPTR(LI_CORE_OPTION_SERVER_NAME).string));
				else
					g_string_append_len(str, GSTR_LEN(req->uri.host));
				break;
			case AL_FORMAT_HOSTNAME:
				if (req->uri.host->len)
					g_string_append_len(str, GSTR_LEN(req->uri.host));
				else
					g_string_append_c(str, '-');
				break;
			case AL_FORMAT_CONNECTION_STATUS:
				/* was request completed? */
				if (vr->con->in->is_closed && vr->con->raw_out->is_closed && 0 == vr->con->raw_out->length)
					g_string_append_c(str, 'X');
				else
					g_string_append_c(str, vr->con->keep_alive ? '+' : '-');
				break;
			case AL_FORMAT_BYTES_IN:
				li_string_append_int(str, vr->con->stats.bytes_in);
				break;
			case AL_FORMAT_BYTES_OUT:
				li_string_append_int(str, vr->con->stats.bytes_out);
				break;
			default:
				/* not implemented:
				{ 'C', FALSE, AL_FORMAT_COOKIE }
				{ 't', FALSE, AL_FORMAT_TIME }, (partially implemented)
				*/
				g_string_append_c(str, '?');
				break;
			}
		} else {
			/* append normal string */
			g_string_append_len(str, GSTR_LEN(e->key));
		}
	}

	g_string_free(tmp_gstr, TRUE);

	return str;
}

static void al_handle_vrclose(liVRequest *vr, liPlugin *p) {
	/* VRequest closed, log it */
	GString *msg;
	liResponse *resp = &vr->response;
	liLog *log = OPTIONPTR(AL_OPTION_ACCESSLOG).ptr;
	GArray *format = OPTIONPTR(AL_OPTION_ACCESSLOG_FORMAT).list;

	UNUSED(p);

	if (LI_VRS_CLEAN == vr->state || resp->http_status == 0 || !log || !format)
		/* if status code is zero, it means the connection was closed while in keep alive state or similar and no logging is needed */
		return;

	msg = al_format_log(vr, p->data, format);

	g_string_append_len(msg, CONST_STR_LEN("\r\n"));
	li_log_write(vr->con->srv, log, msg);
}



static void al_option_accesslog_free(liServer *srv, liPlugin *p, size_t ndx, gpointer oval) {
	UNUSED(p);
	UNUSED(ndx);

	if (!oval) return;

	li_log_unref(srv, oval);
}

static gboolean al_option_accesslog_parse(liServer *srv, liPlugin *p, size_t ndx, liValue *val, gpointer *oval) {
	liLog *log;

	UNUSED(p);
	UNUSED(ndx);

	if (!val) {
		/* default */
		return TRUE;
	}

	if (val->type != LI_VALUE_STRING) {
		ERROR(srv, "accesslog option expects a string as parameter, %s given", li_value_type_string(val->type));
		return FALSE;
	}

	log = li_log_new(srv, li_log_type_from_path(val->data.string), val->data.string);

	*oval = log;

	return TRUE;
}

static void al_option_accesslog_format_free(liServer *srv, liPlugin *p, size_t ndx, gpointer oval) {
	GArray *arr;
	guint i;

	UNUSED(srv);
	UNUSED(p);
	UNUSED(ndx);

	if (!oval) return;

	arr = oval;

	for (i = 0; i < arr->len; i++) {
		al_format_entry *afe = &g_array_index(arr, al_format_entry, i);
		if (afe->key)
			g_string_free(afe->key, TRUE);
	}

	g_array_free(arr, TRUE);
}

static gboolean al_option_accesslog_format_parse(liServer *srv, liPlugin *p, size_t ndx, liValue *val, gpointer *oval) {
	GArray *arr;

	UNUSED(p);
	UNUSED(ndx);

	if (!val) {
		/* default */
		return TRUE;
	}

	if (val->type != LI_VALUE_STRING) {
		ERROR(srv, "accesslog.format option expects a string as parameter, %s given", li_value_type_string(val->type));
		return FALSE;
	}

	arr = al_parse_format(srv, val->data.string);

	if (!arr) {
		ERROR(srv, "%s", "failed to parse accesslog format");
		return FALSE;
	}

	*oval = arr;

	return TRUE;
}


static const liPluginOptionPtr optionptrs[] = {
	{ "accesslog", LI_VALUE_NONE, NULL, al_option_accesslog_parse, al_option_accesslog_free },
	{ "accesslog.format", LI_VALUE_STRING, "%h %V %u %t \"%r\" %>s %b \"%{Referer}i\" \"%{User-Agent}i\"", al_option_accesslog_format_parse, al_option_accesslog_format_free },

	{ NULL, 0, NULL, NULL, NULL }
};

static const liPluginAction actions[] = {
	{ NULL, NULL, NULL }
};

static const liPluginSetup setups[] = {
	{ NULL, NULL, NULL }
};


static void plugin_accesslog_free(liServer *srv, liPlugin *p) {
	UNUSED(srv);

	g_slice_free(al_data, p->data);
}

static void plugin_accesslog_init(liServer *srv, liPlugin *p, gpointer userdata) {
	al_data *ald;

	UNUSED(srv); UNUSED(userdata);

	p->free = plugin_accesslog_free;
	p->optionptrs = optionptrs;
	p->actions = actions;
	p->setups = setups;
	p->handle_vrclose = al_handle_vrclose;

	ald = g_slice_new0(al_data);
	ald->ts_ndx = li_server_ts_format_add(srv, g_string_new_len(CONST_STR_LEN("[%d/%b/%Y:%H:%M:%S %z]")));
	p->data = ald;
}

LI_API gboolean mod_accesslog_init(liModules *mods, liModule *mod) {
	MODULE_VERSION_CHECK(mods);

	mod->config = li_plugin_register(mods->main, "mod_accesslog", plugin_accesslog_init, NULL);

	return mod->config != NULL;
}

LI_API gboolean mod_accesslog_free(liModules *mods, liModule *mod) {
	UNUSED(mods); UNUSED(mod);

	if (mod->config)
		li_plugin_free(mods->main, mod->config);

	return TRUE;
}

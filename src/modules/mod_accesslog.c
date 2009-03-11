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
 *     accesslog <file> - log target
 *         type: string
 *     accesslog.format <format> - log target
 *         type: string
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

LI_API gboolean mod_accesslog_init(modules *mods, module *mod);
LI_API gboolean mod_accesslog_free(modules *mods, module *mod);

struct al_data {
	guint ts_ndx_gmtime;
};
typedef struct al_data al_data;

enum {
	AL_OPTION_ACCESSLOG,
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
		if (str->str[i] == '"') {
			g_string_append_len(log, CONST_STR_LEN("\""));
		} else if (str->str[i] >= ' ' && str->str[i] <= '~') {
			/* printable chars */
			g_string_append_c(log, str->str[i]);
		} else if (str->str[i] == '\n') {
			g_string_append_len(log, CONST_STR_LEN("\\n"));
		} else if (str->str[i] == '\r') {
			g_string_append_len(log, CONST_STR_LEN("\\r"));
		} else if (str->str[i] == '\t') {
			g_string_append_len(log, CONST_STR_LEN("\\t"));
		} else {
			/* non printable char => \xHH */
			gchar hh[5] = {'\\','x',0,0,0};
			gchar h = str->str[i] / 16;
			hh[2] = (h > 9) ? (h - 10 + 'A') : (h + '0');
			h = str->str[i] % 16;
			hh[3] = (h > 9) ? (h - 10 + 'A') : (h + '0');
			g_string_append_len(log, &hh[0], 4);
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

static GArray *al_parse_format(server *srv, GString *formatstr) {
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

static GString *al_format_log(connection *con, al_data *ald, GArray *format) {
	GString *str = g_string_sized_new(256);
	vrequest *vr = con->mainvr;
	response *resp = &vr->response;
	request *req = &vr->request;
	physical *phys = &vr->physical;
	gchar *tmp_str = NULL;
	GString *tmp_gstr = g_string_sized_new(128);
	GString *tmp_gstr2;
	guint len = 0;

	for (guint i = 0; i < format->len; i++) {
		al_format_entry *e = &g_array_index(format, al_format_entry, i);
		if (e->type == AL_ENTRY_FORMAT) {
			switch (e->format.type) {
			case AL_FORMAT_PERCENT:
				g_string_append_c(str, '%');
				break;
			case AL_FORMAT_REMOTE_ADDR:
				g_string_append_len(str, GSTR_LEN(con->remote_addr_str));
				break;
			case AL_FORMAT_LOCAL_ADDR:
				g_string_append_len(str, GSTR_LEN(con->local_addr_str));
				break;
			case AL_FORMAT_BYTES_RESPONSE:
				g_string_append_printf(str, "%jd", vr->out->bytes_out);
				break;
			case AL_FORMAT_BYTES_RESPONSE_CLF:
				if (vr->out->bytes_out)
					g_string_append_printf(str, "%jd", vr->out->bytes_out);
				else
					g_string_append_c(str, '-');
				break;
			case AL_FORMAT_ENV:
				tmp_str = getenv(e->key->str);
				if (tmp_str)
					g_string_append(str, tmp_str);
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
				http_header_get_fast(tmp_gstr, req->headers, GSTR_LEN(e->key));
				al_append_escaped(str, tmp_gstr);
				break;
			case AL_FORMAT_METHOD:
				g_string_append_len(str, GSTR_LEN(req->http_method_str));
				break;
			case AL_FORMAT_RESPONSE_HEADER:
				http_header_get_fast(tmp_gstr, resp->headers, GSTR_LEN(e->key));
				al_append_escaped(str, tmp_gstr);
				break;
			case AL_FORMAT_QUERY_STRING:
				al_append_escaped(str, req->uri.query);
				break;
			case AL_FORMAT_FIRST_LINE:
				g_string_append_len(str, GSTR_LEN(req->http_method_str));
				g_string_append_c(str, ' ');
				al_append_escaped(str, req->uri.orig_path);
				if (req->uri.query->len) {
					g_string_append_c(str, '?');
					al_append_escaped(str, req->uri.query);
				}
				g_string_append_c(str, ' ');
				tmp_str = http_version_string(req->http_version, &len);
				g_string_append_len(str, tmp_str, len);
				break;
			case AL_FORMAT_STATUS_CODE:
				g_string_append_printf(str, "%d", resp->http_status);
				break;
			case AL_FORMAT_TIME:
				/* todo: implement format string */
				tmp_gstr2 = worker_current_timestamp(con->wrk, ald->ts_ndx_gmtime);
				g_string_append_len(str, GSTR_LEN(tmp_gstr2));
				break;
			case AL_FORMAT_AUTHED_USER:
				/* TODO: implement ;) */
				g_string_append_c(str, '-');
				break;
			case AL_FORMAT_PATH:
				g_string_append_len(str, GSTR_LEN(req->uri.path));
				break;
			case AL_FORMAT_SERVER_NAME:
				if (CORE_OPTION(CORE_OPTION_LOG).string)
					g_string_append_len(str, GSTR_LEN(req->uri.host));
				else
					g_string_append_len(str, GSTR_LEN(CORE_OPTION(CORE_OPTION_SERVER_NAME).string));
				break;
			case AL_FORMAT_HOSTNAME:
				g_string_append_len(str, GSTR_LEN(req->uri.host));
				break;
			case AL_FORMAT_CONNECTION_STATUS:
				/* was request completed? */
				if (con->in->is_closed && con->raw_out->is_closed && 0 == con->raw_out->length)
					g_string_append_c(str, 'X');
				else
					g_string_append_c(str, con->keep_alive ? '+' : '-');
				break;
			case AL_FORMAT_BYTES_IN:
				g_string_append_printf(str, "%"G_GUINT64_FORMAT, con->stats.bytes_in);
				break;
			case AL_FORMAT_BYTES_OUT:
				g_string_append_printf(str, "%"G_GUINT64_FORMAT, con->stats.bytes_out);
				break;
			default:
				/* not implemented:
				{ 'C', FALSE, AL_FORMAT_COOKIE }
				{ 'D', FALSE, AL_FORMAT_DURATION_MICROSECONDS }
				{ 'p', FALSE, AL_FORMAT_LOCAL_PORT },
				{ 't', FALSE, AL_FORMAT_TIME },
				{ 'T', FALSE, AL_FORMAT_DURATION_SECONDS },
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

static void al_handle_close(connection *con, plugin *p) {
	/* connection closed, log it */
	GString *msg;
	response *resp = &con->mainvr->response;
	log_t *log = _OPTION(con->mainvr, p, AL_OPTION_ACCESSLOG).ptr;
	GArray *format = _OPTION(con->mainvr, p, AL_OPTION_ACCESSLOG_FORMAT).list;

	UNUSED(p);

	if (resp->http_status == 0 || !log || !format)
		/* if status code is zero, it means the connection was closed while in keep alive state or similar and no logging is needed */
		return;

	msg = al_format_log(con, p->data, format);

	g_string_append_len(msg, CONST_STR_LEN("\r\n"));
	log_write(con->srv, log, msg);
}



static void al_option_accesslog_free(server *srv, plugin *p, size_t ndx, option_value oval) {
	UNUSED(p);
	UNUSED(ndx);

	if (!oval.ptr) return;

	log_free(srv, oval.ptr);
}

static gboolean al_option_accesslog_parse(server *srv, plugin *p, size_t ndx, value *val, option_value *oval) {
	log_t *log;

	UNUSED(p);
	UNUSED(ndx);

	if (!val) {
		/* default */
		return TRUE;
	}

	log = log_new(srv, log_type_from_path(val->data.string), val->data.string);

	oval->ptr = log;

	return TRUE;
}

static void al_option_accesslog_format_free(server *srv, plugin *p, size_t ndx, option_value oval) {
	GArray *arr;

	UNUSED(srv);
	UNUSED(p);
	UNUSED(ndx);

	if (!oval.ptr) return;

	arr = oval.list;

	for (guint i = 0; i < arr->len; i++) {
		al_format_entry *afe = &g_array_index(arr, al_format_entry, i);
		if (afe->key)
			g_string_free(afe->key, TRUE);
	}

	g_array_free(arr, TRUE);
}

static gboolean al_option_accesslog_format_parse(server *srv, plugin *p, size_t ndx, value *val, option_value *oval) {
	GArray *arr;

	UNUSED(p);
	UNUSED(ndx);

	if (!val) {
		/* default */
		return TRUE;
	}

	arr = al_parse_format(srv, val->data.string);

	if (!arr) {
		ERROR(srv, "%s", "failed to parse accesslog format");
		return FALSE;
	}

	oval->list = arr;

	return TRUE;
}


static const plugin_option options[] = {
	{ "accesslog", VALUE_NONE, NULL, al_option_accesslog_parse, al_option_accesslog_free },
	{ "accesslog.format", VALUE_STRING, NULL, al_option_accesslog_format_parse, al_option_accesslog_format_free },

	{ NULL, 0, NULL, NULL, NULL }
};

static const plugin_action actions[] = {
	{ "al", NULL },

	{ NULL, NULL }
};

static const plugin_setup setups[] = {
	{ NULL, NULL }
};


static void plugin_accesslog_free(server *srv, plugin *p) {
	UNUSED(srv);

	g_slice_free(al_data, p->data);
}

static void plugin_accesslog_init(server *srv, plugin *p) {
	al_data *ald;

	UNUSED(srv);

	p->free = plugin_accesslog_free;
	p->options = options;
	p->actions = actions;
	p->setups = setups;
	p->handle_close = al_handle_close;

	ald = g_slice_new0(al_data);
	ald->ts_ndx_gmtime = server_ts_format_add(srv, g_string_new_len(CONST_STR_LEN("[%d/%b/%Y:%H:%M:%S +0000]")));
	p->data = ald;
}

LI_API gboolean mod_accesslog_init(modules *mods, module *mod) {
	UNUSED(mod);

	MODULE_VERSION_CHECK(mods);

	mod->config = plugin_register(mods->main, "mod_accesslog", plugin_accesslog_init);

	return mod->config != NULL;
}

LI_API gboolean mod_accesslog_free(modules *mods, module *mod) {
	UNUSED(mods); UNUSED(mod);

	if (mod->config)
		plugin_free(mods->main, mod->config);

	return TRUE;
}

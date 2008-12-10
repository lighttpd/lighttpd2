/*
 * mod_status - display server status
 *
 * Description:
 *     mod_status can display a page with statistics like requests, traffic and active connections
 *     it can be customized with different stylesheets (css)
 *
 * Setups:
 *     none
 * Options:
 *     status.css <name|url> - set the stylesheet to use
 *         type: string; values: "default", "blue" or a url to an external css file
 * Actions:
 *     status.page           - returns the status page to the client
 *
 * Example config:
 *     req.path == "/status" {
 *         status.css = "http://mydomain/status.css";
 *         status.page;
 *     }
 *
 * Todo:
 *     - handle race condition when connection is gone while collecting data (needs per connection plugin data)
 *
 * Author:
 *     Copyright (c) 2008 Thomas Porzelt
 * License:
 *     MIT, see COPYING file in the lighttpd 2 tree
 */

#include <lighttpd/base.h>
#include <lighttpd/collect.h>


/* html snippet constants */
static const gchar header[] =
	"<?xml version=\"1.0\" encoding=\"iso-8859-1\"?>\n"
	"<!DOCTYPE html PUBLIC \"-//W3C//DTD XHTML 1.0 Transitional//EN\"\n"
	"         \"http://www.w3.org/TR/xhtml1/DTD/xhtml1-transitional.dtd\">\n"
	"<html xmlns=\"http://www.w3.org/1999/xhtml\" xml:lang=\"en\" lang=\"en\">\n"
	"	<head>\n"
	"		<title>Lighttpd Status</title>\n";
static const gchar html_top[] =
	"		<div class=\"header\">Lighttpd Server Status</div>\n"
	"		<div class=\"spacer\">\n"
	"			<strong>Hostname</strong>: <span>%s</span>"
	"			<strong>Uptime</strong>: <span>%s</span>\n"
	"			<strong>Started at</strong>: <span>%s</span>\n"
	"			<strong>Version</strong>: <span>" PACKAGE_VERSION " (" __DATE__ " " __TIME__ ")</span>\n"
	"		</div>\n";
static const gchar html_worker_th[] =
	"		<table cellspacing=\"0\">\n"
	"			<tr>\n"
	"				<th style=\"width: 100px;\"></th>\n"
	"				<th>Requests</th>\n"
	"				<th>Traffic in</th>\n"
	"				<th>Traffic out</th>\n"
	"				<th>Active connections</th>\n"
	"			</tr>\n";
static const gchar html_worker_th_avg[] =
	"		<table cellspacing=\"0\">\n"
	"			<tr>\n"
	"				<th style=\"width: 100px;\"></th>\n"
	"				<th>Requests / s</th>\n"
	"				<th>Traffic in / s</th>\n"
	"				<th>Traffic out / s</th>\n"
	"				<th>Active connections</th>\n"
	"			</tr>\n";
static const gchar html_worker_row[] =
	"			<tr class=\"%s\">\n"
	"				<td class=\"left\">%s</td>\n"
	"				<td>%s (%" G_GUINT64_FORMAT "%%)</td>\n"
	"				<td>%s (%" G_GUINT64_FORMAT "%%)</td>\n"
	"				<td>%s (%" G_GUINT64_FORMAT "%%)</td>\n"
	"				<td>%u (%u%%)</td>\n"
	"			</tr>\n";
static const gchar html_worker_row_avg[] =
	"			<tr class=\"%s\">\n"
	"				<td class=\"left\">%s</td>\n"
	"				<td>%s</td>\n"
	"				<td>%s</td>\n"
	"				<td>%s</td>\n"
	"				<td>%u</td>\n"
	"			</tr>\n";
static const gchar html_connections_th[] =
	"		<table cellspacing=\"0\">\n"
	"			<tr>\n"
	"				<th class=\"left\" style=\"width: 200px;\">Client</th>\n"
	"				<th>State</th>\n"
	"				<th>Host</th>\n"
	"				<th class=\"left\" style=\"width: 250px;\">Path</th>\n"
	"				<th style=\"width: 100px;\">Duration</th>\n"
	"				<th style=\"width: 150px;\">Traffic in/out</th>\n"
	"				<th style=\"width: 150px;\">Traffic in/out / s</th>\n"
	"			</tr>\n";
static const gchar html_connections_row[] =
	"			<tr>\n"
	"				<td  class=\"left\">%s</td>\n"
	"				<td>%s</td>\n"
	"				<td>%s</td>\n"
	"				<td class=\"left\">%s</td>\n"
	"				<td>%s</td>\n"
	"				<td>%s / %s</td>\n"
	"				<td>%s / %s</td>\n"
	"			</tr>\n";
static const gchar css_default[] =
	"		<style type=\"text/css\">\n"
	"			body { margin: 0; padding: 0; font-family: \"lucida grande\",tahoma,verdana,arial,sans-serif; font-size: 12px; }\n"
	"			.header { padding: 5px; background-color: #6D84B4; font-size: 16px; color: white; border: 1px solid #3B5998; font-weight: bold; }\n"
	"			.spacer { background-color: #F2F2F2; border-bottom: 1px solid #CCC; padding: 5px; }\n"
	"			.spacer span { padding-right: 25px; }\n"
	"			.title { margin-left: 6px; margin-top: 25px; margin-bottom: 5px; }\n"
	"			table { margin-left: 5px; border: 1px solid #CCC; }\n"
	"			th { font-weight: normal; padding: 3px; width: 175px; background-color: #E0E0E0;\n"
	"			border-bottom: 1px solid #BABABA; border-right: 1px solid #BABABA; border-top: 1px solid #FEFEFE; }\n"
	"			td { text-align: right; padding: 3px; border-bottom: 1px solid #F0F0F0; border-right: 1px solid #F8F8F8; }\n"
	"			.left { text-align: left; }\n"
	"			.totals td { border-top: 1px solid #DDDDDD; }\n"
	"		</style>\n";
/* blue theme by nitrox */
static const gchar css_blue[] =
	"		<style type=\"text/css\">\n"
	"			body { margin: 0; padding: 0; font-family: \"lucida grande\",tahoma,verdana,arial,sans-serif; font-size: 12px; background-color: #6d84b4; }\n"
	"			.header { padding: 5px; background-color: #6D84B4; font-size: 16px; color: white; border: 1px solid #3B5998; font-weight: bold; }\n"
	"			.spacer { background-color: #F2F2F2; border-bottom: 1px solid #CCC; padding: 5px; }\n"
	"			.spacer span { padding-right: 25px; }\n"
	"			.title { margin-left: 5px; margin-top: 25px; margin-bottom: 5px; }\n"
	"			table { margin-left: 5px; border: 1px solid #CCC; }\n"
	"			th { font-weight: normal; padding: 3px; width: 175px; background-color: #E0E0E0;\n"
	"			border-bottom: 1px solid #BABABA; border-right: 1px solid #BABABA; border-top: 1px solid #FEFEFE; }\n"
	"			td { text-align: right; padding: 3px; border-bottom: 1px solid #F0F0F0; border-right: 1px solid #F8F8F8; }\n"
	"			.left { text-align: left; }\n"
	"			.totals td { border-top: 1px solid #DDDDDD; }\n"
	"		</style>\n";


struct mod_status_wrk_data;
typedef struct mod_status_wrk_data mod_status_wrk_data;

struct mod_status_con_data;
typedef struct mod_status_con_data mod_status_con_data;

struct mod_status_con_data {
	guint worker_ndx;
	connection_state_t state;
	GString *remote_addr_str, *local_addr_str;
	gboolean is_ssl, keep_alive;
	GString *host, *path;
	ev_tstamp ts;
	guint64 bytes_in;
	guint64 bytes_out;
	guint64 bytes_in_5s_diff;
	guint64 bytes_out_5s_diff;
};

struct mod_status_wrk_data {
	guint worker_ndx;
	statistics_t stats;
	GArray *connections;
};


/* the CollectFunc */
static gpointer status_collect_func(worker *wrk, gpointer fdata) {
	UNUSED(fdata);
	mod_status_wrk_data *sd = g_slice_new(mod_status_wrk_data);
	sd->stats = wrk->stats;
	sd->worker_ndx = wrk->ndx;
	/* gather connection info */
	sd->connections = g_array_sized_new(FALSE, TRUE, sizeof(mod_status_con_data), wrk->connections_active);
	g_array_set_size(sd->connections, wrk->connections_active);
	for (guint i = 0; i < wrk->connections_active; i++) {
		connection *c = g_array_index(wrk->connections, connection*, i);
		mod_status_con_data *cd = &g_array_index(sd->connections, mod_status_con_data, i);
		cd->is_ssl = c->is_ssl;
		cd->keep_alive = c->keep_alive;
		cd->remote_addr_str = g_string_new_len(GSTR_LEN(c->remote_addr_str));
		cd->local_addr_str = g_string_new_len(GSTR_LEN(c->local_addr_str));
		cd->host = g_string_new_len(GSTR_LEN(c->mainvr->request.uri.host));
		cd->path = g_string_new_len(GSTR_LEN(c->mainvr->request.uri.path));
		cd->state = c->state;
		cd->ts = c->ts;
		cd->bytes_in = c->stats.bytes_in;
		cd->bytes_out = c->stats.bytes_out;
		cd->bytes_in_5s_diff = c->stats.bytes_in_5s_diff;
		cd->bytes_out_5s_diff = c->stats.bytes_out_5s_diff;
	}
	return sd;
}

/* the CollectCallback */
static void status_collect_cb(gpointer cbdata, gpointer fdata, GPtrArray *result, gboolean complete) {
	UNUSED(fdata);
	vrequest *vr = cbdata;


	if (complete) {
		GString *css;
		GString *tmpstr;
		guint total_connections = 0;

		VR_TRACE(vr, "finished collecting data: %s", complete ? "complete" : "not complete");
		vr->response.http_status = 200;

		/* we got everything */
		statistics_t totals = {
			G_GUINT64_CONSTANT(0), G_GUINT64_CONSTANT(0), G_GUINT64_CONSTANT(0), G_GUINT64_CONSTANT(0),
			G_GUINT64_CONSTANT(0), G_GUINT64_CONSTANT(0), G_GUINT64_CONSTANT(0), G_GUINT64_CONSTANT(0),
			G_GUINT64_CONSTANT(0), G_GUINT64_CONSTANT(0), G_GUINT64_CONSTANT(0),
			0, 0, G_GUINT64_CONSTANT(0), 0, 0
		};
		GString *html = g_string_sized_new(8 * 1024);

		/* calculate total stats over all workers */
		for (guint i = 0; i < result->len; i++) {
			mod_status_wrk_data *sd = g_ptr_array_index(result, i);

			totals.bytes_out += sd->stats.bytes_out;
			totals.bytes_in += sd->stats.bytes_in;
			totals.requests += sd->stats.requests;
			totals.actions_executed += sd->stats.actions_executed;
			total_connections += sd->connections->len;

			totals.requests_5s_diff += sd->stats.requests_5s_diff;
			totals.bytes_in_5s_diff += sd->stats.bytes_in_5s_diff;
			totals.bytes_out_5s_diff += sd->stats.bytes_out_5s_diff;
			totals.active_cons_cum += sd->stats.active_cons_cum;
			totals.active_cons_5s += sd->stats.active_cons_5s;
		}

		g_string_append_len(html, header, sizeof(header)-1);

		/* css */
		css = _OPTION(vr, ((plugin*)fdata), 0).string;
		if (!css || !css->len) /* default css */
			g_string_append_len(html, css_default, sizeof(css_default)-1);
		else if (g_str_equal(css->str, "blue")) /* blue css */
			g_string_append_len(html, css_blue, sizeof(css_blue)-1);
		else /* external css */
			g_string_append_printf(html, "		<link rel=\"stylesheet\" rev=\"stylesheet\" href=\"%s\" media=\"screen\" />\n", css->str);

		g_string_append_len(html, CONST_STR_LEN(
			"	</head>\n"
			"	<body>\n"
		));

		tmpstr = counter_format2((guint64)(CUR_TS(vr->con->wrk) - vr->con->srv->started), COUNTER_TIME, -1);
		g_string_append_printf(html, html_top,
			vr->request.uri.host->str,
			tmpstr->str,
			vr->con->srv->started_str->str
		);


		/* worker information, absolute values */
		{
			GString *count_req, *count_bin, *count_bout;

			g_string_append_len(html, CONST_STR_LEN("		<div class=\"title\"><strong>Absolute stats</strong> (since start)</div>\n"));

			g_string_append_len(html, html_worker_th, sizeof(html_worker_th)-1);

			#define PERCENTAGE(x, y) (y ? (x * 100 / y) : 0)
			for (guint i = 0; i < result->len; i++) {
				mod_status_wrk_data *sd = g_ptr_array_index(result, i);
				count_req = counter_format2(sd->stats.requests, COUNTER_UNITS, -1);
				count_bin = counter_format2(sd->stats.bytes_in, COUNTER_BYTES, 2);
				count_bout = counter_format2(sd->stats.bytes_out, COUNTER_BYTES, 2);
				g_string_printf(tmpstr, "Worker #%u", i+1);
				g_string_append_printf(html, html_worker_row, "", tmpstr->str,
				count_req->str, PERCENTAGE(sd->stats.requests, totals.requests),
				count_bin->str, PERCENTAGE(sd->stats.bytes_in, totals.bytes_in),
				count_bout->str, PERCENTAGE(sd->stats.bytes_out, totals.bytes_out),
				sd->connections->len, PERCENTAGE(sd->connections->len, total_connections));
				g_string_free(count_req, TRUE);
				g_string_free(count_bin, TRUE);
				g_string_free(count_bout, TRUE);
			}
			#undef PERCENTAGE

			count_req = counter_format2(totals.requests, COUNTER_UNITS, -1);
			count_bin = counter_format2(totals.bytes_in, COUNTER_BYTES, 2);
			count_bout = counter_format2(totals.bytes_out, COUNTER_BYTES, 2);
			g_string_append_printf(html, html_worker_row, "totals", "Total",
				count_req->str, G_GUINT64_CONSTANT(100),
				count_bin->str, G_GUINT64_CONSTANT(100),
				count_bout->str, G_GUINT64_CONSTANT(100),
				total_connections, 100);
			g_string_append_len(html, CONST_STR_LEN("		</table>\n"));
			g_string_free(count_req, TRUE);
			g_string_free(count_bin, TRUE);
			g_string_free(count_bout, TRUE);
		}

		/* worker information, avg values */
		{
			GString *count_req, *count_bin, *count_bout;
			guint uptime = CUR_TS(vr->con->wrk) - vr->con->srv->started;
			if (!uptime)
				uptime = 1;

			g_string_append_len(html, CONST_STR_LEN("<div class=\"title\"><strong>Average stats</strong> (since start)</div>\n"));

			g_string_append_len(html, html_worker_th_avg, sizeof(html_worker_th_avg)-1);

			#define PERCENTAGE(x) (sd->stat ## x ? (sd->stat ## x * 100 / total ## x) : 0)
			for (guint i = 0; i < result->len; i++) {
				mod_status_wrk_data *sd = g_ptr_array_index(result, i);

				count_req = counter_format2(sd->stats.requests / uptime, COUNTER_UNITS, -1);
				count_bin = counter_format2(sd->stats.bytes_in / uptime, COUNTER_BYTES, 2);
				count_bout = counter_format2(sd->stats.bytes_out / uptime, COUNTER_BYTES, 2);
				g_string_printf(tmpstr, "Worker #%u", i+1);
				g_string_append_printf(html, html_worker_row_avg, "", tmpstr->str,
					count_req->str,
					count_bin->str,
					count_bout->str,
					(guint)(sd->stats.active_cons_cum / uptime)
				);
				g_string_free(count_req, TRUE);
				g_string_free(count_bin, TRUE);
				g_string_free(count_bout, TRUE);
			}
			#undef PERCENTAGE

			count_req = counter_format2(totals.requests / uptime, COUNTER_UNITS, -1);
			count_bin = counter_format2(totals.bytes_in / uptime, COUNTER_BYTES, 2);
			count_bout = counter_format2(totals.bytes_out / uptime, COUNTER_BYTES, 2);
			g_string_append_printf(html, html_worker_row_avg, "totals", "Total",
				count_req->str,
				count_bin->str,
				count_bout->str,
				(guint)(totals.active_cons_cum / uptime)
			);
			g_string_append_len(html, CONST_STR_LEN("		</table>\n"));
			g_string_free(count_req, TRUE);
			g_string_free(count_bin, TRUE);
			g_string_free(count_bout, TRUE);
		}


		/* worker information, 5 seconds avg values */
		{
			GString *count_req, *count_bin, *count_bout;
			time_t uptime = CUR_TS(vr->con->wrk) - vr->con->srv->started;
			if (!uptime)
				uptime = 1;

			g_string_append_len(html, CONST_STR_LEN("<div class=\"title\"><strong>Average stats</strong> (5 seconds)</div>\n"));

			g_string_append_len(html, html_worker_th_avg, sizeof(html_worker_th_avg)-1);

			#define PERCENTAGE(x) (sd->stat ## x ? (sd->stat ## x * 100 / total ## x) : 0)
			for (guint i = 0; i < result->len; i++) {
				mod_status_wrk_data *sd = g_ptr_array_index(result, i);

				count_req = counter_format2(sd->stats.requests_5s_diff / G_GUINT64_CONSTANT(5), COUNTER_UNITS, -1);
				count_bin = counter_format2(sd->stats.bytes_in_5s_diff / G_GUINT64_CONSTANT(5), COUNTER_BYTES, 2);
				count_bout = counter_format2(sd->stats.bytes_out_5s_diff / G_GUINT64_CONSTANT(5), COUNTER_BYTES, 2);
				g_string_printf(tmpstr, "Worker #%u", i+1);
				g_string_append_printf(html, html_worker_row_avg, "", tmpstr->str,
					count_req->str,
					count_bin->str,
					count_bout->str,
					sd->stats.active_cons_5s
				);
				g_string_free(count_req, TRUE);
				g_string_free(count_bin, TRUE);
				g_string_free(count_bout, TRUE);
			}
			#undef PERCENTAGE

			count_req = counter_format2(totals.requests_5s_diff / G_GUINT64_CONSTANT(5), COUNTER_UNITS, -1);
			count_bin = counter_format2(totals.bytes_in_5s_diff / G_GUINT64_CONSTANT(5), COUNTER_BYTES, 2);
			count_bout = counter_format2(totals.bytes_out_5s_diff / G_GUINT64_CONSTANT(5), COUNTER_BYTES, 2);
			g_string_append_printf(html, html_worker_row_avg, "totals", "Total",
				count_req->str,
				count_bin->str,
				count_bout->str,
				totals.active_cons_5s
			);
			g_string_append_len(html, CONST_STR_LEN("		</table>\n"));
			g_string_free(count_req, TRUE);
			g_string_free(count_bin, TRUE);
			g_string_free(count_bout, TRUE);
		}

		/* list connections */
		{
			GString *ts, *bytes_in, *bytes_out, *bytes_in_5s, *bytes_out_5s;
			g_string_append_len(html, CONST_STR_LEN("<div class=\"title\"><strong>Active connections</strong></div>\n"));
			g_string_append_len(html, html_connections_th, sizeof(html_connections_th)-1);
			for (guint i = 0; i < result->len; i++) {
				mod_status_wrk_data *sd = g_ptr_array_index(result, i);
				for (guint j = 0; j < sd->connections->len; j++) {
					mod_status_con_data *cd = &g_array_index(sd->connections, mod_status_con_data, j);

					ts = counter_format2((guint64)(CUR_TS(vr->con->wrk) - cd->ts), COUNTER_TIME, -1);
					bytes_in = counter_format2(cd->bytes_in, COUNTER_BYTES, 1);
					bytes_in_5s = counter_format2(cd->bytes_in_5s_diff / G_GUINT64_CONSTANT(5), COUNTER_BYTES, 1);
					bytes_out = counter_format2(cd->bytes_out, COUNTER_BYTES, 1);
					bytes_out_5s = counter_format2(cd->bytes_out_5s_diff / G_GUINT64_CONSTANT(5), COUNTER_BYTES, 1);

					g_string_append_printf(html, html_connections_row,
						cd->remote_addr_str->str,
						connection_state_str(cd->state),
						cd->host->str,
						cd->path->str,
						ts->str,
						bytes_in->str,
						bytes_out->str,
						bytes_in_5s->str,
						bytes_out_5s->str
					);

					g_string_free(cd->remote_addr_str, TRUE);
					g_string_free(cd->local_addr_str, TRUE);
					g_string_free(cd->host, TRUE);
					g_string_free(cd->path, TRUE);
					g_string_free(ts, TRUE);
					g_string_free(bytes_in, TRUE);
					g_string_free(bytes_in_5s, TRUE);
					g_string_free(bytes_out, TRUE);
					g_string_free(bytes_out_5s, TRUE);
				}
				g_array_free(sd->connections, TRUE);
			}
			g_string_append_len(html, CONST_STR_LEN("		</table>\n"));
		}

		/* free stats */
		for (guint i = 0; i < result->len; i++) {
			mod_status_wrk_data *sd = g_ptr_array_index(result, i);
			g_slice_free(mod_status_wrk_data, sd);
		}

		g_string_append_len(html, CONST_STR_LEN(
			" </body>\n"
			"</html>\n"
		));
		chunkqueue_append_string(vr->con->out, html);
		http_header_overwrite(vr->response.headers, CONST_STR_LEN("Content-Type"), CONST_STR_LEN("text/html"));
		g_string_free(tmpstr, TRUE);

		vrequest_handle_direct(vr);
		vrequest_joblist_append(vr);
	} else {
		/* something went wrong, client may have dropped the connection */
		CON_ERROR(vr->con, "%s", "collect request didn't end up complete");
		vrequest_error(vr);
	}
}

static handler_t status_page_handle(vrequest *vr, gpointer param, gpointer *context) {
	UNUSED(param);

	if (vr->state == VRS_HANDLE_REQUEST_HEADERS) {
		collect_info *ci;
		VR_TRACE(vr, "%s", "collecting stats...");
		/* abuse fdata as pointer to plugin */
		ci = collect_start(vr->con->wrk, status_collect_func, param, NULL, status_collect_cb, vr);
		*context = ci;
		return HANDLER_WAIT_FOR_EVENT;
	}

	return HANDLER_GO_ON;
}

static handler_t status_page_cleanup(vrequest *vr, gpointer param, gpointer context) {
	collect_info *ci = context;

	UNUSED(vr);
	UNUSED(param);

	collect_break(ci);

	return HANDLER_GO_ON;
}

static action* status_page(server *srv, plugin* p, value *val) {
	UNUSED(srv); UNUSED(p); UNUSED(val);
	return action_new_function(status_page_handle, status_page_cleanup, NULL, p);
}



static const plugin_option options[] = {
	{ "status.css", VALUE_STRING, NULL, NULL, NULL },

	{ NULL, 0, NULL, NULL, NULL }
};

static const plugin_action actions[] = {
	{ "status.page", status_page },

	{ NULL, NULL }
};

static const plugin_setup setups[] = {
	{ NULL, NULL }
};


static void plugin_status_init(server *srv, plugin *p) {
	UNUSED(srv);

	p->options = options;
	p->actions = actions;
	p->setups = setups;
}


LI_API gboolean mod_status_init(modules *mods, module *mod) {
	UNUSED(mod);

	MODULE_VERSION_CHECK(mods);

	mod->config = plugin_register(mods->main, "mod_status", plugin_status_init);

	return mod->config != NULL;
}

LI_API gboolean mod_status_free(modules *mods, module *mod) {
	UNUSED(mods); UNUSED(mod);

	if (mod->config)
		plugin_free(mods->main, mod->config);

	return TRUE;
}

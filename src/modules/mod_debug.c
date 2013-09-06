/*
 * mod_debug - utilities to debug lighttpd
 *
 * Description:
 *     mod_debug offers various utilities to aid you debug a problem.
 *
 * Setups:
 *     none
 * Options:
 *     none
 * Actions:
 *     debug.show_connections;
 *         - shows a page similar to the one from mod_status, listing all active connections
 *         - by specifying one or more "connection ids" via querystring (parameter "con"),
 *           one can request additional debug output for specific connections
 *     debug.profiler_dump;
 *         - dumps all allocated memory to the profiler output file if profiling enabled (LIGHTY_PROFILE_MEM=profiler.log)
 *
 * Example config:
 *     if req.path == "/debug/connections" { debug.show_connections; }
 *
 * Tip:
 *     none
 *
 * Todo:
 *     - prettier output
 *     - more detailed output
 *     - more debug actions (for other stuff than connections)
 *
 * Author:
 *     Copyright (c) 2009-2011 Thomas Porzelt
 * License:
 *     MIT, see COPYING file in the lighttpd 2 tree
 */

#include <lighttpd/base.h>

#include <lighttpd/lighttpd-glue.h>

#ifdef WITH_PROFILER
# include <lighttpd/profiler.h>
#endif

LI_API gboolean mod_debug_init(liModules *mods, liModule *mod);
LI_API gboolean mod_debug_free(liModules *mods, liModule *mod);

struct mod_debug_detailed_t {
	liConnection con;
};
typedef struct mod_debug_detailed_t mod_debug_detailed_t;

struct mod_debug_data_t {
	guint wrk_ndx;
	guint con_ndx;
	liConnection *con;
	liWaitQueueElem io_timeout_elem;
	gint fd;
	liConnectionState state;
	GString *remote_addr_str, *local_addr_str;
	gboolean is_ssl, keep_alive;
	GString *host, *path, *query;
	liHttpMethod method;
	goffset request_size;
	goffset response_size;
	li_tstamp ts_started;
	guint64 bytes_in;
	guint64 bytes_out;
	guint64 bytes_in_5s_diff;
	guint64 bytes_out_5s_diff;
	GString *detailed;
};
typedef struct mod_debug_data_t mod_debug_data_t;

struct mod_debug_job_t {
	liVRequest *vr;
	gpointer *context;
	liPlugin *p;
	struct {
		guint wrk_ndx;
		guint con_ndx;
		gint fd;
		GString *remote_addr_str;
	} detailed;
};
typedef struct mod_debug_job_t mod_debug_job_t;

/* the CollectFunc */
static gpointer debug_collect_func(liWorker *wrk, gpointer fdata) {
	GArray *cons;
	guint len;
	mod_debug_job_t *job = fdata;

	/* gather connection info */
	cons = g_array_sized_new(FALSE, TRUE, sizeof(mod_debug_data_t), wrk->connections_active);
	g_array_set_size(cons, wrk->connections_active);

	for (guint i = 0; i < wrk->connections_active; i++) {
		liConnection *c = g_array_index(wrk->connections, liConnection*, i);
		mod_debug_data_t *cd = &g_array_index(cons, mod_debug_data_t, i);
		cd->wrk_ndx = wrk->ndx;
		cd->con_ndx = i;
		cd->con = c;
		cd->io_timeout_elem = c->io_timeout_elem;
		cd->fd = -1;
		cd->is_ssl = c->info.is_ssl;
		cd->keep_alive = c->info.keep_alive;
		cd->remote_addr_str = g_string_new_len(GSTR_LEN(c->info.remote_addr_str));
		cd->local_addr_str = g_string_new_len(GSTR_LEN(c->info.local_addr_str));
		cd->host = g_string_new_len(GSTR_LEN(c->mainvr->request.uri.host));
		cd->path = g_string_new_len(GSTR_LEN(c->mainvr->request.uri.path));
		cd->query = g_string_new_len(GSTR_LEN(c->mainvr->request.uri.query));
		cd->method = c->mainvr->request.http_method;
		cd->request_size = c->mainvr->request.content_length;
		cd->response_size = (NULL != c->mainvr->backend_source) ? c->mainvr->backend_source->out->bytes_out : 0;
		cd->state = c->state;
		cd->ts_started = c->ts_started;
		cd->bytes_in = c->info.stats.bytes_in;
		cd->bytes_out = c->info.stats.bytes_out;
		cd->bytes_in_5s_diff = c->info.stats.bytes_in_5s_diff;
		cd->bytes_out_5s_diff = c->info.stats.bytes_out_5s_diff;

		if (job->detailed.remote_addr_str) {
			if (job->detailed.wrk_ndx == wrk->ndx && job->detailed.con_ndx == i &&
				job->detailed.fd == cd->fd && g_string_equal(job->detailed.remote_addr_str, c->info.remote_addr_str)) {

				cd->detailed = g_string_sized_new(1023);
				g_string_append_printf(cd->detailed, "<pre>connection* @ %p = {\n", (void*)cd->con);
				g_string_append_printf(cd->detailed, "	fd = %d,\n", cd->fd);
				g_string_append_printf(cd->detailed, "	remote_addr_str = \"%s\",\n", cd->remote_addr_str->str);
				g_string_append_printf(cd->detailed, "	local_addr_str = \"%s\",\n", cd->local_addr_str->str);
				g_string_append_printf(cd->detailed, "	is_ssl = \"%s\",\n", cd->is_ssl ? "true" : "false");
				g_string_append_printf(cd->detailed, "	keep_alive = \"%s\",\n", cd->keep_alive ? "true" : "false");
				g_string_append_printf(cd->detailed, "	state = \"%s\",\n", li_connection_state_str(cd->state));
				g_string_append_printf(cd->detailed, "	ts_started = %f,\n", cd->ts_started);
				g_string_append_printf(cd->detailed,
					"	io_timeout_elem = {\n"
					"		queued = %s,\n"
					"		ts = %f,\n"
					"		prev = %p,\n"
					"		next = %p,\n"
					"		data = %p,\n"
					"	}\n",
					cd->io_timeout_elem.queued ? "true":"false",
					cd->io_timeout_elem.ts,
					(void*)cd->io_timeout_elem.prev, (void*)cd->io_timeout_elem.next,
					cd->io_timeout_elem.data
				);
				g_string_append_printf(cd->detailed,
					"	stats = {\n"
					"		bytes_in = %"G_GUINT64_FORMAT",\n"
					"		bytes_out = %"G_GUINT64_FORMAT"\n"
					"	}\n",
					cd->bytes_in, cd->bytes_out
				);
				g_string_append_printf(cd->detailed,
					"	mainvr = {\n"
					"		request = {\n"
					"			method = \"%s\"\n"
					"			host = \"%s\"\n"
					"			path = \"%s\"\n"
					"			query = \"%s\"\n"
					"		}\n"
					"	}\n",
					li_http_method_string(cd->method, &len),
					cd->host->str,
					cd->path->str,
					cd->query->str
				);
				g_string_append_len(cd->detailed, CONST_STR_LEN("}</pre>"));
			}
		}
	}

	return cons;
}

/* the CollectCallback */
static void debug_collect_cb(gpointer cbdata, gpointer fdata, GPtrArray *result, gboolean complete) {
	mod_debug_job_t *job = cbdata;
	liVRequest *vr;
	GString *html;

	UNUSED(fdata);

	if (!complete) {
		/* someone called li_collect_break, so we don't need any vrequest handling here. just free the result data */
		guint i, j;

		for (i = 0; i < result->len; i++) {
			GArray *cons = g_ptr_array_index(result, i);
			for (j = 0; j < cons->len; j++) {
				mod_debug_data_t *cd = &g_array_index(cons, mod_debug_data_t, j);

				g_string_free(cd->remote_addr_str, TRUE);
				g_string_free(cd->local_addr_str, TRUE);
				g_string_free(cd->host, TRUE);
				g_string_free(cd->path, TRUE);
				g_string_free(cd->query, TRUE);
				if (cd->detailed)
					g_string_free(cd->detailed, TRUE);
			}

			g_array_free(cons, TRUE);
		}

		if (job->detailed.remote_addr_str)
			g_string_free(job->detailed.remote_addr_str, TRUE);
		g_slice_free(mod_debug_job_t, job);

		return;
	}

	vr = job->vr;
	/* clear context so it doesn't get cleaned up anymore */
	*(job->context) = NULL;

	html = g_string_sized_new(2047);

	g_string_append_len(html, CONST_STR_LEN("<html>\n<head>\n<title>Lighttpd mod_debug</title>\n"
		"<style>a { color: blue; }</style>\n"
		"</head>\n<body>\n"));

	/* list connections */
	{
		guint i, j;
		GString *duration = g_string_sized_new(15);

		g_string_append_printf(html, "<p>now: %f<br>io timeout watcher active/repeat: %s/%f<br></p>\n",
			li_cur_ts(vr->wrk), li_event_active(&vr->wrk->io_timeout_queue.timer) ? "yes":"no",
			vr->wrk->io_timeout_queue.timer.libevmess.timer.repeat
		);

		g_string_append_len(html, CONST_STR_LEN("<table><tr><th>Client</th><th>Duration</th><th></th></tr>\n"));

		for (i = 0; i < result->len; i++) {
			GArray *cons = g_ptr_array_index(result, i);

			for (j = 0; j < cons->len; j++) {
				mod_debug_data_t *d = &g_array_index(cons, mod_debug_data_t, j);

				li_counter_format((guint64)(li_cur_ts(vr->wrk) - d->ts_started), COUNTER_TIME, duration);
				g_string_append_printf(html, "<tr><td>%s</td><td style=\"text-align:right;\">%s</td><td style=\"padding-left:10px;\"><a href=\"?%u_%u_%d_%s\">debug</a></td></tr>\n",
					d->remote_addr_str->str,
					duration->str,
					d->wrk_ndx, d->con_ndx, d->fd, d->remote_addr_str->str
				);

				if (d->detailed) {
					g_string_append_printf(html, "<tr><td colspan=\"3\">%s</td></tr>\n", d->detailed->str);
					g_string_free(d->detailed, TRUE);
				}

				g_string_free(d->remote_addr_str, TRUE);
				g_string_free(d->local_addr_str, TRUE);
				g_string_free(d->host, TRUE);
				g_string_free(d->path, TRUE);
				g_string_free(d->query, TRUE);
			}

			g_array_free(cons, TRUE);
		}

		g_string_append_len(html, CONST_STR_LEN("</table>\n"));
		g_string_free(duration, TRUE);
	}

	g_string_append_len(html, CONST_STR_LEN("</body>\n</html>\n"));

	if (li_vrequest_handle_direct(vr)) {
		li_http_header_overwrite(vr->response.headers, CONST_STR_LEN("Content-Type"), CONST_STR_LEN("text/html; charset=utf-8"));
		vr->response.http_status = 200;

		li_chunkqueue_append_string(vr->direct_out, html);
		li_vrequest_joblist_append(vr);
	} else {
		g_string_free(html, TRUE);
	}

	if (job->detailed.remote_addr_str)
		g_string_free(job->detailed.remote_addr_str, TRUE);
	g_slice_free(mod_debug_job_t, job);
}

static liHandlerResult debug_show_connections_cleanup(liVRequest *vr, gpointer param, gpointer context) {
	liCollectInfo *ci = (liCollectInfo*) context;

	UNUSED(vr);
	UNUSED(param);

	li_collect_break(ci);

	return LI_HANDLER_GO_ON;
}

static liHandlerResult debug_show_connections(liVRequest *vr, gpointer param, gpointer *context) {
	liPlugin *p = (liPlugin*) param;

	switch (vr->request.http_method) {
	case LI_HTTP_METHOD_GET:
	case LI_HTTP_METHOD_HEAD:
		break;
	default:
		return LI_HANDLER_GO_ON;
	}

	if (NULL != *context)
		return LI_HANDLER_WAIT_FOR_EVENT;

	if (!li_vrequest_is_handled(vr)) {
		liCollectInfo *ci;
		mod_debug_job_t *j = g_slice_new0(mod_debug_job_t);
		j->vr = vr;
		j->context = context;
		j->p = p;

		if (vr->request.uri.query->len) {
			/* querystring = <wrk_ndx>_<con_ndx>_<con_fd>_<remote_addr_str> */
			j->detailed.remote_addr_str = g_string_sized_new(vr->request.uri.query->len);
			sscanf(vr->request.uri.query->str, "%u_%u_%i_%s", &(j->detailed.wrk_ndx),
				&(j->detailed.con_ndx), &(j->detailed.fd), j->detailed.remote_addr_str->str);
			g_string_set_size(j->detailed.remote_addr_str, strlen(j->detailed.remote_addr_str->str));
		}

		VR_DEBUG(vr, "%s", "collecting debug info...");

		ci = li_collect_start(vr->wrk, debug_collect_func, j, debug_collect_cb, j);
		*context = ci; /* may be NULL */
	}

	return (*context) ? LI_HANDLER_WAIT_FOR_EVENT : LI_HANDLER_GO_ON;
}

static liAction* debug_show_connections_create(liServer *srv, liWorker *wrk, liPlugin* p, liValue *val, gpointer userdata) {
	UNUSED(wrk); UNUSED(userdata);

	if (!li_value_is_nothing(val)) {
		ERROR(srv, "%s", "debug.show_connections doesn't expect any parameters");
		return NULL;
	}

	return li_action_new_function(debug_show_connections, debug_show_connections_cleanup, NULL, p);
}

#ifdef WITH_PROFILER
static liHandlerResult debug_profiler_dump(liVRequest *vr, gpointer param, gpointer *context) {
	gint minsize = GPOINTER_TO_INT(param);

	UNUSED(vr); UNUSED(context);

	if (!getenv("LIGHTY_PROFILE_MEM"))
		return LI_HANDLER_GO_ON;

	li_profiler_dump(minsize);

	return LI_HANDLER_GO_ON;
}

static liAction* debug_profiler_dump_create(liServer *srv, liWorker *wrk, liPlugin* p, liValue *val, gpointer userdata) {
	gpointer ptr;
	UNUSED(wrk); UNUSED(p); UNUSED(userdata);

	val = li_value_get_single_argument(val);

	if (LI_VALUE_NONE == li_value_type(val)) {
		ptr = GINT_TO_POINTER(10240);
	} else if (LI_VALUE_NUMBER == li_value_type(val)) {
		ptr = GINT_TO_POINTER(val->data.number);
	} else {
		ERROR(srv, "%s", "debug.profiler_dump takes an optional integer (minsize) as parameter");
		return NULL;
	}

	return li_action_new_function(debug_profiler_dump, NULL, NULL, ptr);
}
#endif


static const liPluginOption options[] = {
	{ NULL, 0, 0, NULL }
};

static const liPluginAction actions[] = {
	{ "debug.show_connections", debug_show_connections_create, NULL },
#ifdef WITH_PROFILER
	{ "debug.profiler_dump", debug_profiler_dump_create, NULL },
#endif

	{ NULL, NULL, NULL }
};

static const liPluginSetup setups[] = {
	{ NULL, NULL, NULL }
};


static void plugin_debug_init(liServer *srv, liPlugin *p, gpointer userdata) {
	UNUSED(srv); UNUSED(userdata);

	p->options = options;
	p->actions = actions;
	p->setups = setups;
}


gboolean mod_debug_init(liModules *mods, liModule *mod) {
	UNUSED(mod);

	MODULE_VERSION_CHECK(mods);

	mod->config = li_plugin_register(mods->main, "mod_debug", plugin_debug_init, NULL);

	return mod->config != NULL;
}

gboolean mod_debug_free(liModules *mods, liModule *mod) {
	if (mod->config)
		li_plugin_free(mods->main, mod->config);

	return TRUE;
}

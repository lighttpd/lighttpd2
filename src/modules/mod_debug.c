/*
 * mod_debug - utilities to debug lighttpd
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

struct plugin_debug_worker_data {
	liWorker *wrk;
	struct plugin_debug_data *pd;
	liEventAsync stop_listen;
	liEventTimer stop_listen_timeout;
};
typedef struct plugin_debug_worker_data plugin_debug_worker_data;

struct plugin_debug_data {
	int stop_listen_timeout_seconds;
	plugin_debug_worker_data *worker_data;
};
typedef struct plugin_debug_data plugin_debug_data;


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

/* if show_all is FALSE only active events that keep the loop alive are shown */
static void log_events(liWorker *wrk, liLogContext *context, gboolean show_all) {
	GList *lnk;

	for (lnk = wrk->loop.watchers.head; NULL != lnk; lnk = lnk->next) {
		liEventBase *base = LI_CONTAINER_OF(lnk, liEventBase, link_watchers);
		gboolean active = li_event_active_(base);

		if (show_all || (active && base->keep_loop_alive)) {
			_ERROR(wrk->srv, wrk, context,
				"Event listener for worker %i: '%s' (%s %s)%s",
				wrk->ndx,
				base->event_name,
				active ? "active" : "inactive",
				li_event_type_string(base->type),
				base->keep_loop_alive
					? (active
						? ""
						: " [doesn't keep loop alive]")
					: " [does never keep loop alive]");
		}
	}
}

/* if show_all is FALSE only active events that keep the loop alive are shown */
static void format_events(GString *out, liWorker *wrk, gboolean show_all) {
	GList *lnk;
	g_string_truncate(out, 0);

	for (lnk = wrk->loop.watchers.head; NULL != lnk; lnk = lnk->next) {
		liEventBase *base = LI_CONTAINER_OF(lnk, liEventBase, link_watchers);
		gboolean active = li_event_active_(base);

		if (show_all || (active && base->keep_loop_alive)) {
			g_string_append_printf(out,
				"Event listener for worker %i: '%s' (%s %s)%s\n",
				wrk->ndx,
				base->event_name,
				active ? "active" : "inactive",
				li_event_type_string(base->type),
				base->keep_loop_alive
					? (active
						? ""
						: " [doesn't keep loop alive]")
					: " [does never keep loop alive]");
		}
	}
}

struct collect_events_job {
	liVRequest *vr;
	gpointer *context;
	gboolean show_all;
};
typedef struct collect_events_job collect_events_job;

/* the CollectFunc */
static gpointer debug_show_events_func(liWorker *wrk, gpointer fdata) {
	collect_events_job *job = fdata;
	GString *out = g_string_sized_new(0);

	format_events(out, wrk, job->show_all);

	return out;
}

/* the CollectCallback */
static void debug_show_events_cb(gpointer cbdata, gpointer fdata, GPtrArray *result, gboolean complete) {
	collect_events_job *job = fdata;
	liVRequest *vr;
	UNUSED(cbdata);

	if (complete) {
		vr = job->vr;
		/* clear context so it doesn't get cleaned up anymore */
		*(job->context) = NULL;

		if (li_vrequest_handle_direct(vr)) {
			guint i;

			li_http_header_overwrite(vr->response.headers, CONST_STR_LEN("Content-Type"), CONST_STR_LEN("text/plain; charset=utf-8"));
			vr->response.http_status = 200;

			for (i = 0; i < result->len; i++) {
				GString *str = g_ptr_array_index(result, i);
				g_ptr_array_index(result, i) = NULL;
				li_chunkqueue_append_string(vr->direct_out, str);
			}

			li_vrequest_joblist_append(vr);
		}
	}

	{
		guint i;

		for (i = 0; i < result->len; i++) {
			GString *str = g_ptr_array_index(result, i);
			if (NULL != str) g_string_free(str, TRUE);
		}

		g_slice_free(collect_events_job, job);
	}
}

static liHandlerResult debug_show_events_cleanup(liVRequest *vr, gpointer param, gpointer context) {
	liCollectInfo *ci = (liCollectInfo*) context;

	UNUSED(vr);
	UNUSED(param);

	li_collect_break(ci);

	return LI_HANDLER_GO_ON;
}

static liHandlerResult debug_show_events(liVRequest *vr, gpointer param, gpointer *context) {
	UNUSED(param);

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
		collect_events_job *job = g_slice_new0(collect_events_job);
		job->vr = vr;
		job->context = context;
		job->show_all = TRUE;

		VR_DEBUG(vr, "%s", "collecting events info...");

		ci = li_collect_start(vr->wrk, debug_show_events_func, job, debug_show_events_cb, NULL);
		*context = ci; /* may be NULL */
	}

	return (*context) ? LI_HANDLER_WAIT_FOR_EVENT : LI_HANDLER_GO_ON;
}

static liAction* debug_show_events_create(liServer *srv, liWorker *wrk, liPlugin* p, liValue *val, gpointer userdata) {
	UNUSED(wrk); UNUSED(userdata); UNUSED(p);

	if (!li_value_is_nothing(val)) {
		ERROR(srv, "%s", "debug.show_events doesn't expect any parameters");
		return NULL;
	}

	return li_action_new_function(debug_show_events, debug_show_events_cleanup, NULL, NULL);
}

static gboolean debug_show_events_after_shutdown(liServer *srv, liPlugin* p, liValue *val, gpointer userdata) {
	plugin_debug_data *pd = p->data;
	UNUSED(userdata);

	val = li_value_get_single_argument(val);

	if (LI_VALUE_NUMBER != li_value_type(val)) {
		ERROR(srv, "debug.show_events_after_shutdown expected number, got %s", li_value_type_string(val));
		return FALSE;
	}

	pd->stop_listen_timeout_seconds = val->data.number;

	return TRUE;
}

static const liPluginOption options[] = {
	{ NULL, 0, 0, NULL }
};

static const liPluginAction actions[] = {
	{ "debug.show_connections", debug_show_connections_create, NULL },
#ifdef WITH_PROFILER
	{ "debug.profiler_dump", debug_profiler_dump_create, NULL },
#endif
	{ "debug.show_events", debug_show_events_create, NULL },

	{ NULL, NULL, NULL }
};

static const liPluginSetup setups[] = {
	{ "debug.show_events_after_shutdown", debug_show_events_after_shutdown, NULL },

	{ NULL, NULL, NULL }
};

static void plugin_debug_stop_listen_timeout(liEventBase *watcher, int events) {
	plugin_debug_worker_data *pwd = LI_CONTAINER_OF(li_event_timer_from(watcher), plugin_debug_worker_data, stop_listen_timeout);
	UNUSED(events);

	ERROR(pwd->wrk->srv, "Couldn't suspend yet, checking events for worker %i:", pwd->wrk->ndx);
	log_events(pwd->wrk, NULL, FALSE);
}

static void plugin_debug_worker_stop_listen(liEventBase *watcher, int events) {
	plugin_debug_worker_data *pwd = LI_CONTAINER_OF(li_event_async_from(watcher), plugin_debug_worker_data, stop_listen);
	plugin_debug_data *pd = pwd->pd;
	UNUSED(events);

	if (!li_event_attached(&pwd->stop_listen_timeout)) {
		li_event_attach(&pwd->wrk->loop, &pwd->stop_listen_timeout);
		li_event_timer_once(&pwd->stop_listen_timeout, pd->stop_listen_timeout_seconds);
	}
}

static void plugin_debug_prepare_worker(liServer *srv, liPlugin *p, liWorker *wrk) {
	plugin_debug_data *pd = p->data;
	plugin_debug_worker_data *pwd;
	UNUSED(srv);

	if (pd->stop_listen_timeout_seconds < 0) return;

	pwd = &pd->worker_data[wrk->ndx];
	pwd->pd = pd;
	pwd->wrk = wrk;

	li_event_async_init(&wrk->loop, "mod_debug stop_listen", &pwd->stop_listen, plugin_debug_worker_stop_listen);
	li_event_timer_init(NULL, "mod_debug stop_listen_timeout", &pwd->stop_listen_timeout, plugin_debug_stop_listen_timeout);
	li_event_set_keep_loop_alive(&pwd->stop_listen_timeout, FALSE);
}

static void plugin_debug_prepare(liServer *srv, liPlugin *p) {
	plugin_debug_data *pd = p->data;

	if (pd->stop_listen_timeout_seconds >= 0) {
		pd->worker_data = g_new0(plugin_debug_worker_data, srv->worker_count);
	}
}

static void plugin_debug_stop_listen(liServer *srv, liPlugin *p) {
	plugin_debug_data *pd = p->data;

	if (NULL != pd->worker_data) {
		unsigned int i;
		for (i = 0; i < srv->worker_count; ++i) {
			plugin_debug_worker_data *pwd = &pd->worker_data[i];
			if (NULL == pwd->wrk) continue;
			li_event_async_send(&pwd->stop_listen);
		}
	}
}

static void plugin_debug_free(liServer *srv, liPlugin *p) {
	plugin_debug_data *pd = p->data;

	if (NULL != pd->worker_data) {
		unsigned int i;
		for (i = 0; i < srv->worker_count; ++i) {
			plugin_debug_worker_data *pwd = &pd->worker_data[i];
			li_event_clear(&pwd->stop_listen);
			li_event_clear(&pwd->stop_listen_timeout);
		}
		g_free(pd->worker_data);
	}
	g_free(pd);
}

static void plugin_debug_init(liServer *srv, liPlugin *p, gpointer userdata) {
	plugin_debug_data *pd = g_malloc0(sizeof(plugin_debug_data));
	UNUSED(srv); UNUSED(userdata);

	p->options = options;
	p->actions = actions;
	p->setups = setups;

	p->data = pd;

	pd->stop_listen_timeout_seconds = -1; /* disabled by default */

	p->free = plugin_debug_free;
	p->handle_stop_listen = plugin_debug_stop_listen;
	p->handle_prepare = plugin_debug_prepare;
	p->handle_prepare_worker = plugin_debug_prepare_worker;
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

/*
 * mod_progress - track connection progress (state) via a unique identifier
 *
 * Description:
 *     mod_progress lets you track connection progress (or rather state) using a lookup table
 *     in which connections are registered via a random unique identifier specified with the request
 *
 * Setups:
 *     progress.ttl <duration>;
 *         - Sets the time to live in seconds for entries after a disconnect in the internal lookup table.
 *           Defaults to 30 seconds.
 * Options:
 *     progress.debug = <true|false>;
 *         - if true, debug info is written to the log
 *     progress.methods = <methods>;
 *         - list of methods that should be tracked, defaults to POST only. Example: progress.methods = ("GET", "POST");
 * Actions:
 *     progress.track;
 *         - tracks the current connection if the X-Progress-ID querystring key is supplied
 *     progress.show [format];
 *         - returns the current progress/state of
 *         - [format] can be one of "legacy", "json" or "jsonp" . See example responses below.
 *           Defaults to "json".
 *
 * Examples responses:
 *     - legacy format
 *       new Object({"state": "running"", "received": 123456, "sent": 0, "request_size": 200000, "response_size": 0})
 *     - json format
 *       {"state": "running", "received": 123456, "sent": 0, "request_size": 200000, "response_size": 0}
 *     - jsonp format (function name specified via X-Progress-Callback querystring key, defaults to "progress")
 *       progress({"state": "running", "received": 123456, "sent": 0, "request_size": 200000, "response_size": 0})
 *
 * Possible response objects:
 *     - {"state": "unknown"}
 *     - {"state": "running", "received": <bytes_recived>, "sent": <bytes_sent>, "request_size": <bytes>, "response_size": <bytes>}
 *     - {"state": "done", "received": <bytes_recived>, "sent": <bytes_sent>, "request_size": <bytes>, "response_size": <bytes>}
 *     - {"state": "error", "status": <http_status>}
 *
 * Example config:
 *     if req.path == "/upload.php" {
 *         progress.track;
 *     } else if req.path == "/progress" {
 *         progress.show;
 *     }
 *
 *     The config snippet will track all POST requests (uploads) to /upload.php?X-Progress-ID=<id>
 *     where <id> is a random unqiue ID.
 *     The progress of a particular request can then be fetched via /progress?X-Progress-ID=<id>
 *     where <id> is the ID specified with the POST request to /upload.php
 *
 * Tip:
 *     none
 *
 * Todo:
 *     - stop waitqueues
 *     - "dump" format to return an array of all tracked requests?
 *     - "template" format to return for example HTML?
 *
 * Author:
 *     Copyright (c) 2010 Thomas Porzelt
 * License:
 *     MIT, see COPYING file in the lighttpd 2 tree
 */

#include <lighttpd/base.h>

#include <lighttpd/lighttpd-glue.h>

LI_API gboolean mod_progress_init(liModules *mods, liModule *mod);
LI_API gboolean mod_progress_free(liModules *mods, liModule *mod);

typedef enum {
	PROGRESS_FORMAT_LEGACY,
	PROGRESS_FORMAT_JSON,
	PROGRESS_FORMAT_JSONP,
	PROGRESS_FORMAT_DUMP
} mod_progress_format;

typedef struct mod_progress_node mod_progress_node;
typedef struct mod_progress_data mod_progress_data;
typedef struct mod_progress_worker_data mod_progress_worker_data;
typedef struct mod_progress_show_param mod_progress_show_param;
typedef struct mod_progress_job mod_progress_job;

struct mod_progress_node {
	gchar *id; /* unique id */
	liWaitQueueElem timeout_queue_elem;
	mod_progress_worker_data *worker_data;
	liVRequest *vr; /* null in case of tombstone. otherwise the following vars will not be valid! */
	goffset request_size;
	goffset response_size;
	guint64 bytes_in;
	guint64 bytes_out;
	gint status_code;
};

struct mod_progress_data {
	liPlugin *p;
	guint ttl;
	mod_progress_worker_data *worker_data;
};

struct mod_progress_worker_data {
	mod_progress_data *pd;
	guint wrk_ndx;
	GHashTable *hash_table;
	liWaitQueue timeout_queue; /* each worker has its own timeout queue */
};

struct mod_progress_show_param {
	liPlugin *p;
	mod_progress_format format;
};

struct mod_progress_job {
	liVRequest *vr;
	gpointer *context;
	gboolean debug;
	mod_progress_format format;

	gchar *id;
	liPlugin *p;
};

/* global data */
// static mod_progress_data progress_data;

static void progress_timeout_callback(liWaitQueue *wq, gpointer data) {
	mod_progress_worker_data *wrk_data = data;
	liWaitQueueElem *wqe;
	mod_progress_node *node;

	while ((wqe = li_waitqueue_pop(wq)) != NULL) {
		node = wqe->data;
		g_hash_table_remove(wrk_data->hash_table, node->id);
	}

	li_waitqueue_update(wq);
}

static void progress_hashtable_free_callback(gpointer data) {
	mod_progress_node *node = data;
	mod_progress_worker_data *wd = node->worker_data;

	if (node->vr) {
		g_ptr_array_index(node->vr->plugin_ctx, wd->pd->p->id) = NULL;
	}

	li_waitqueue_remove(&wd->timeout_queue, &(node->timeout_queue_elem));
	g_free(node->id);
	g_slice_free(mod_progress_node, node);
}

static void progress_vrclose(liVRequest *vr, liPlugin *p) {
	mod_progress_node *node = (mod_progress_node*) g_ptr_array_index(vr->plugin_ctx, p->id);
	mod_progress_data *pd = p->data;

	if (node) {
		/* connection is being tracked, replace with tombstone */
		node->vr = NULL;
		node->request_size = vr->request.content_length;
		node->response_size = vr->out->bytes_out;
		node->bytes_in = vr->vr_in->bytes_in;
		node->bytes_out = MAX(0, vr->vr_out->bytes_out - vr->coninfo->out_queue_length);
		node->status_code = vr->response.http_status;
		li_waitqueue_push(&pd->worker_data[vr->wrk->ndx].timeout_queue, &(node->timeout_queue_elem));
	}
}

static liHandlerResult progress_track(liVRequest *vr, gpointer param, gpointer *context) {
	gchar *id;
	guint id_len;
	liPlugin *p = (liPlugin*) param;
	gboolean debug = _OPTION(vr, p, 0).boolean;
	gint methods = _OPTION(vr, p, 1).number;
	mod_progress_data *pd = p->data;

	UNUSED(context);

	if (!(methods & (1 << vr->request.http_method))) {
		/* method not tracked */
	} else if (g_ptr_array_index(vr->plugin_ctx, p->id)) {
		/* already tracked */
		VR_WARNING(vr, "%s", "progress.track: already tracking request");
	} else if (li_querystring_find(vr->request.uri.query, CONST_STR_LEN("X-Progress-Id"), &id, &id_len) && id_len <= 128) {
		/* progress id found, start tracking of connection */
		mod_progress_node *node = g_slice_new0(mod_progress_node);
		node->timeout_queue_elem.data = node;
		node->id = g_strndup(id, id_len);
		node->worker_data = &pd->worker_data[vr->wrk->ndx];
		node->vr = vr;
		g_ptr_array_index(vr->plugin_ctx, pd->p->id) = node;
		g_hash_table_replace(node->worker_data->hash_table, node->id, node);

		if (debug)
			VR_DEBUG(vr, "progress.track: tracking progress with id \"%s\"", node->id);
	} else if (debug) {
		VR_DEBUG(vr, "%s", "progress.track: X-Progress-Id parameter not found, cannot track request");
	}

	return LI_HANDLER_GO_ON;
}

static liAction* progress_track_create(liServer *srv, liWorker *wrk, liPlugin* p, liValue *val, gpointer userdata) {
	UNUSED(wrk); UNUSED(userdata);

	if (val) {
		ERROR(srv, "%s", "progress.show doesn't expect any parameters");
		return NULL;
	}

	return li_action_new_function(progress_track, NULL, NULL, p);
}


/* the CollectFunc */
static gpointer progress_collect_func(liWorker *wrk, gpointer fdata) {
	mod_progress_node *node, *node_new;
	mod_progress_job *job = fdata;
	mod_progress_data *pd = job->p->data;

	node = g_hash_table_lookup(pd->worker_data[wrk->ndx].hash_table, job->id);

	if (!node)
		return NULL;

	node_new = g_slice_new0(mod_progress_node);

	if (node->vr) {
		/* copy live data */
		node_new->vr = node->vr;
		node_new->request_size = node->vr->request.content_length;
		node_new->response_size = node->vr->out->bytes_out;
		node_new->bytes_in = node->vr->vr_in->bytes_in;
		node_new->bytes_out = MAX(0, node->vr->vr_out->bytes_out - node->vr->coninfo->out_queue_length);
		node_new->status_code = node->vr->response.http_status;
	} else {
		/* copy dead data */
		*node_new = *node;
	}

	return node_new;
}

/* the CollectCallback */
static void progress_collect_cb(gpointer cbdata, gpointer fdata, GPtrArray *result, gboolean complete) {
	guint i;
	GString *output;
	mod_progress_node *node = NULL;
	mod_progress_job *job = fdata;
	liVRequest *vr = job->vr;
	gboolean debug = job->debug;
	mod_progress_format format = job->format;

	UNUSED(cbdata);

	if (complete) {
		/* clear context so it doesn't get cleaned up anymore */
		*(job->context) = NULL;

		for (i = 0; i < result->len; i++) {
			node = g_ptr_array_index(result, i);
			if (node)
				break;
		}

		output = g_string_sized_new(128);

		/* send mime-type. there seems to be no standard for javascript... using the most commong */
		li_http_header_overwrite(vr->response.headers, CONST_STR_LEN("Content-Type"), CONST_STR_LEN("application/x-javascript"));

		if (format == PROGRESS_FORMAT_LEGACY) {
			g_string_append_len(output, CONST_STR_LEN("new Object("));
		} else if (format == PROGRESS_FORMAT_JSONP) {
			gchar *val;
			guint len;

			if (li_querystring_find(vr->request.uri.query, CONST_STR_LEN("X-Progress-Callback"), &val, &len)) {
				/* X-Progress-Callback specified, need to check for xss */
				gchar *c;

				for (c = val; c != val+len; c++) {
					if ((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == '.' || *c == '_')
						continue;
					break;
				}

				/* was there a bad char? */
				if (c != val+len) {
					g_string_append_len(output, CONST_STR_LEN("progress("));
				} else {
					g_string_append_len(output,val, len);
					g_string_append_c(output, '(');
				}
			} else {
				g_string_append_len(output, CONST_STR_LEN("progress("));
			}
		}

		if (!node) {
			/* progress id not known */
			if (debug)
				VR_DEBUG(vr, "progress.show: progress id \"%s\" unknown", job->id);
			
			g_string_append_len(output, CONST_STR_LEN("{\"state\": \"unknown\"}"));
		} else {
			if (debug)
				VR_DEBUG(vr, "progress.show: progress id \"%s\" found", job->id);

			if (node->vr) {
				/* still in progress */
				g_string_append_printf(output,
					"{\"state\": \"running\", \"received\": %"G_GUINT64_FORMAT", \"sent\": %"G_GUINT64_FORMAT", \"request_size\": %"G_GUINT64_FORMAT", \"response_size\": %"G_GUINT64_FORMAT"}",
					node->bytes_in, node->bytes_out, node->request_size, node->response_size
				);
			} else if (node->status_code == 200) {
				/* done, success */
				g_string_append_printf(output,
					"{\"state\": \"done\", \"received\": %"G_GUINT64_FORMAT", \"sent\": %"G_GUINT64_FORMAT", \"request_size\": %"G_GUINT64_FORMAT", \"response_size\": %"G_GUINT64_FORMAT"}",
					node->bytes_in, node->bytes_out, node->request_size, node->response_size
				);
			} else {
				/* done, error */
				g_string_append_printf(output,
					"{\"state\": \"error\", \"status\": %d}",
					node->status_code
				);
			}
		}

		if (format == PROGRESS_FORMAT_LEGACY || format == PROGRESS_FORMAT_JSONP) {
			g_string_append_c(output, ')');
		}

		vr->response.http_status = 200;
		li_chunkqueue_append_string(vr->out, output);
		li_vrequest_handle_direct(vr);
		li_vrequest_joblist_append(vr);
	}

	/* free results */
	for (i = 0; i < result->len; i++) {
		if (g_ptr_array_index(result, i))
			g_slice_free(mod_progress_node, g_ptr_array_index(result, i));
	}

	g_free(job->id);
	g_slice_free(mod_progress_job, job);
}

static liHandlerResult progress_collect_cleanup(liVRequest *vr, gpointer param, gpointer context) {
	liCollectInfo *ci = (liCollectInfo*) context;

	UNUSED(vr);
	UNUSED(param);

	li_collect_break(ci);

	return LI_HANDLER_GO_ON;
}

static liHandlerResult progress_show(liVRequest *vr, gpointer param, gpointer *context) {
	mod_progress_show_param *psp = (mod_progress_show_param*) param;
	gboolean debug = _OPTION(vr, psp->p, 0).boolean;
	gchar *id;
	guint id_len;
	liCollectInfo *ci;
	mod_progress_job *job;

	if (*context)
		return LI_HANDLER_WAIT_FOR_EVENT;

	if (li_vrequest_is_handled(vr))
		return LI_HANDLER_GO_ON;

	if (!li_querystring_find(vr->request.uri.query, CONST_STR_LEN("X-Progress-Id"), &id, &id_len) || id_len == 0 || id_len > 128) {
		if (debug)
			VR_DEBUG(vr, "%s", "progress.show: X-Progress-Id not specified");
		return LI_HANDLER_GO_ON;
	}

	/* start collect job */
	job = g_slice_new(mod_progress_job);
	job->vr = vr;
	job->context = context;
	job->format = psp->format;
	job->debug = debug;

	job->p = psp->p;
	job->id = g_strndup(id, id_len);

	ci = li_collect_start(vr->wrk, progress_collect_func, job, progress_collect_cb, NULL);
	*context = ci; /* may be NULL */
	return ci ? LI_HANDLER_WAIT_FOR_EVENT : LI_HANDLER_GO_ON;
}

static void progress_show_free(liServer *srv, gpointer param) {
	UNUSED(srv);

	g_slice_free(mod_progress_show_param, param);
}

static liAction* progress_show_create(liServer *srv, liWorker *wrk, liPlugin* p, liValue *val, gpointer userdata) {
	mod_progress_show_param *psp;
	mod_progress_format format = PROGRESS_FORMAT_JSON;

	UNUSED(srv);
	UNUSED(wrk);
	UNUSED(userdata);

	if (!val) {
		format = PROGRESS_FORMAT_JSON;
	} else if (val->type == LI_VALUE_STRING) {
		gchar *str = val->data.string->str;
		if (g_str_equal(str, "legacy")) {
			format = PROGRESS_FORMAT_LEGACY;
		} else if (g_str_equal(str, "json")) {
			format = PROGRESS_FORMAT_JSON;
		} else if (g_str_equal(str, "jsonp")) {
			format = PROGRESS_FORMAT_JSONP;
		} else if (g_str_equal(str, "dump")) {
			format = PROGRESS_FORMAT_DUMP;
		} else {
			ERROR(srv, "progress.show: unknown format \"%s\"", str);
			return NULL;
		}
	} else {
		ERROR(srv, "%s", "progress.show expects an optional string as parameter");
		return NULL;
	}

	psp = g_slice_new(mod_progress_show_param);
	psp->format = format;
	psp->p = p;

	return li_action_new_function(progress_show, progress_collect_cleanup, progress_show_free, psp);
}

static gboolean progress_methods_parse(liServer *srv, liWorker *wrk, liPlugin *p, size_t ndx, liValue *val, liOptionValue *oval) {
	GArray *arr;
	guint methods = 0;
	UNUSED(wrk);
	UNUSED(p);
	UNUSED(ndx);

	/* default value */
	if (!val) {
		oval->number = 1 << LI_HTTP_METHOD_POST;
		return TRUE;
	}

	/* Need manual type check, as resulting option type is number */
	if (val->type != LI_VALUE_LIST) {
		ERROR(srv, "progress.methods option expects a list of strings, parameter is of type %s", li_value_type_string(val->type));
		return FALSE;
	}
	arr = val->data.list;
	for (guint i = 0; i < arr->len; i++) {
		liHttpMethod method;
		liValue *v = g_array_index(arr, liValue*, i);
		if (v->type != LI_VALUE_STRING) {
			ERROR(srv, "progress.methods option expects a list of strings, entry #%u is of type %s", i, li_value_type_string(v->type));
			return FALSE;
		}

		method = li_http_method_from_string(GSTR_LEN(v->data.string));
		if (method == LI_HTTP_METHOD_UNSET) {
			ERROR(srv, "progress.methods: unknown method: %s", v->data.string->str);
			return FALSE;
		}

		methods |= 1 << method;
	}

	oval->number = (guint64) methods;
	return TRUE;
}

static gboolean progress_ttl(liServer *srv, liPlugin* p, liValue *val, gpointer userdata) {
	mod_progress_data *pd = p->data;
	UNUSED(userdata);

	if (!val) {
		ERROR(srv, "%s", "progress.ttl expects a number as parameter");
		return FALSE;
	}
	if (val->type != LI_VALUE_NUMBER) {
		ERROR(srv, "expected number, got %s", li_value_type_string(val->type));
		return FALSE;
	}

	pd->ttl = val->data.number;

	return TRUE;
}

static void progress_prepare(liServer *srv, liPlugin *p) {
	mod_progress_data *pd = p->data;
	guint i;

	pd->worker_data = g_slice_alloc0(sizeof(mod_progress_worker_data) * srv->worker_count);
	for (i = 0; i < srv->worker_count; i++) {
		liWorker *wrk = g_array_index(srv->workers, liWorker*, i);

		pd->worker_data[i].pd = pd;
		pd->worker_data[i].wrk_ndx = i;
		pd->worker_data[i].hash_table = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, progress_hashtable_free_callback);
		li_waitqueue_init(&(pd->worker_data[i].timeout_queue), wrk->loop, progress_timeout_callback, pd->ttl, &pd->worker_data[i]);
	}
}


static const liPluginOption options[] = {
	{ "progress.debug", LI_VALUE_BOOLEAN, FALSE, NULL },
	{ "progress.methods", LI_VALUE_LIST, 0, progress_methods_parse },

	{ NULL, 0, 0, NULL }
};

static const liPluginOptionPtr optionptrs[] = {
	{ NULL, 0, NULL, NULL, NULL }
};

static const liPluginAction actions[] = {
	{ "progress.track", progress_track_create, NULL },
	{ "progress.show", progress_show_create, NULL },

	{ NULL, NULL, NULL }
};

static const liPluginSetup setups[] = {
	{ "progress.ttl", progress_ttl, NULL },

	{ NULL, NULL, NULL }
};

static void plugin_progress_free(liServer *srv, liPlugin *p) {
	guint i;
	mod_progress_data *pd = p->data;

	for (i = 0; i < srv->worker_count; i++) {
		g_hash_table_destroy(pd->worker_data[i].hash_table);
	}

	g_slice_free1(sizeof(mod_progress_worker_data) * srv->worker_count, pd->worker_data);
}

static void plugin_progress_init(liServer *srv, liPlugin *p, gpointer userdata) {
	mod_progress_data *pd = g_slice_new0(mod_progress_data);
	UNUSED(srv); UNUSED(userdata);

	p->data = pd;
	pd->p = p;
	pd->ttl = 30;

	p->options = options;
	p->optionptrs = optionptrs;
	p->actions = actions;
	p->setups = setups;

	p->free = plugin_progress_free;
	p->handle_vrclose = progress_vrclose;
	p->handle_prepare = progress_prepare;
}


gboolean mod_progress_init(liModules *mods, liModule *mod) {
	UNUSED(mod);

	MODULE_VERSION_CHECK(mods);

	mod->config = li_plugin_register(mods->main, "mod_progress", plugin_progress_init, NULL);

	return mod->config != NULL;
}

gboolean mod_progress_free(liModules *mods, liModule *mod) {
	if (mod->config)
		li_plugin_free(mods->main, mod->config);

	return TRUE;
}

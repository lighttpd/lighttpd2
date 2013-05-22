
#include <lighttpd/base.h>
#include <lighttpd/plugin_core.h>

static void vrequest_job_cb(liJob *job) {
	liVRequest *vr = LI_CONTAINER_OF(job, liVRequest, job);
	li_vrequest_state_machine(vr);
}

liVRequest* li_vrequest_new(liWorker *wrk, liConInfo *coninfo) {
	liServer *srv = wrk->srv;
	liVRequest *vr = g_slice_new0(liVRequest);

	vr->coninfo = coninfo;
	vr->wrk = wrk;
	vr->state = LI_VRS_CLEAN;

	vr->backend = NULL;
	vr->backend_drain = NULL;
	vr->backend_source = NULL;
	vr->direct_out = NULL;

	vr->plugin_ctx = g_ptr_array_new();
	g_ptr_array_set_size(vr->plugin_ctx, g_hash_table_size(srv->plugins));
	vr->options = g_slice_copy(srv->option_def_values->len * sizeof(liOptionValue), srv->option_def_values->data);
	vr->optionptrs = g_slice_copy(srv->optionptr_def_values->len * sizeof(liOptionPtrValue*), srv->optionptr_def_values->data);
	{
		guint i;
		for (i = 0; i < srv->optionptr_def_values->len; i++) {
			if (vr->optionptrs[i]) {
				g_atomic_int_inc(&vr->optionptrs[i]->refcount);
			}
		}
	}

	li_request_init(&vr->request);
	li_physical_init(&vr->physical);
	li_response_init(&vr->response);
	li_environment_init(&vr->env);

	li_vrequest_filters_init(vr);

	vr->in_buffer_state.flush_limit = -1; /* wait until upload is complete */
	vr->in_buffer_state.split_on_file_chunks = FALSE;

	li_action_stack_init(&vr->action_stack);

	li_job_init(&vr->job, vrequest_job_cb);

	vr->stat_cache_entries = g_ptr_array_sized_new(2);

	vr->throttle.wqueue_elem.data = vr;
	vr->throttle.pool.lnk.data = vr;
	vr->throttle.ip.lnk.data = vr;

	return vr;
}

void li_vrequest_free(liVRequest* vr) {
	liServer *srv = vr->wrk->srv;

	if (NULL != vr->backend_drain) {
		li_stream_reset(vr->backend_drain);
		li_stream_release(vr->backend_drain);
		vr->backend_drain = NULL;
	}
	if (NULL != vr->backend_source) {
		li_stream_reset(vr->backend_source);
		li_stream_release(vr->backend_source);
		vr->backend_source = NULL;
		vr->direct_out = NULL;
	}

	li_action_stack_clear(vr, &vr->action_stack);
	if (vr->state != LI_VRS_CLEAN) {
		li_plugins_handle_vrclose(vr);
		vr->state = LI_VRS_CLEAN;
		vr->backend = NULL;
	}
	g_ptr_array_free(vr->plugin_ctx, TRUE);
	vr->plugin_ctx = NULL;

	li_request_clear(&vr->request);
	li_physical_clear(&vr->physical);
	li_response_clear(&vr->response);
	li_environment_clear(&vr->env);

	li_vrequest_filters_clear(vr);

	li_filter_buffer_on_disk_reset(&vr->in_buffer_state);

	li_job_clear(&vr->job);

	g_slice_free1(srv->option_def_values->len * sizeof(liOptionValue), vr->options);
	{
		guint i;
		for (i = 0; i < srv->optionptr_def_values->len; i++) {
			li_release_optionptr(srv, vr->optionptrs[i]);
		}
	}
	g_slice_free1(srv->optionptr_def_values->len * sizeof(liOptionPtrValue*), vr->optionptrs);

	li_log_context_set(&vr->log_context, NULL);

	while (vr->stat_cache_entries->len > 0 ) {
		liStatCacheEntry *sce = g_ptr_array_index(vr->stat_cache_entries, 0);
		li_stat_cache_entry_release(vr, sce);
	}
	g_ptr_array_free(vr->stat_cache_entries, TRUE);

	g_slice_free(liVRequest, vr);
}

void li_vrequest_reset(liVRequest *vr, gboolean keepalive) {
	liServer *srv = vr->wrk->srv;

	if (NULL != vr->backend_drain) {
		li_stream_disconnect(vr->backend_drain);
		li_stream_release(vr->backend_drain);
		vr->backend_drain = NULL;
	}
	if (NULL != vr->backend_source) {
		if (NULL == vr->backend_source->dest) {
			/* wasn't connected: disconnect source */
			li_stream_disconnect(vr->backend_source);
		}
		li_stream_disconnect_dest(vr->backend_source);
		li_stream_release(vr->backend_source);
		vr->backend_source = NULL;
		vr->direct_out = NULL;
	}

	li_action_stack_reset(vr, &vr->action_stack);
	if (vr->state != LI_VRS_CLEAN) {
		li_plugins_handle_vrclose(vr);
		vr->state = LI_VRS_CLEAN;
		vr->backend = NULL;
	}
	{
		gint len = vr->plugin_ctx->len;
		g_ptr_array_set_size(vr->plugin_ctx, 0);
		g_ptr_array_set_size(vr->plugin_ctx, len);
	}


	/* don't reset request for keep-alive tracking */
	if (!keepalive) li_request_reset(&vr->request);
	li_physical_reset(&vr->physical);
	li_response_reset(&vr->response);
	li_environment_reset(&vr->env);

	li_vrequest_filters_reset(vr);

	li_filter_buffer_on_disk_reset(&vr->in_buffer_state);
	vr->in_buffer_state.flush_limit = -1; /* wait until upload is complete */
	vr->in_buffer_state.split_on_file_chunks = FALSE;

	li_job_reset(&vr->job);

	while (vr->stat_cache_entries->len > 0 ) {
		liStatCacheEntry *sce = g_ptr_array_index(vr->stat_cache_entries, 0);
		li_stat_cache_entry_release(vr, sce);
	}

	memcpy(vr->options, srv->option_def_values->data, srv->option_def_values->len * sizeof(liOptionValue));
	{
		guint i;
		for (i = 0; i < srv->optionptr_def_values->len; i++) {
			liOptionPtrValue *oval = g_array_index(srv->optionptr_def_values, liOptionPtrValue*, i);
			if (vr->optionptrs[i] != oval) {
				li_release_optionptr(srv, vr->optionptrs[i]);
				if (oval)
					g_atomic_int_inc(&oval->refcount);
				vr->optionptrs[i] = oval;
			}
		}
	}

	li_log_context_set(&vr->log_context, NULL);
}

void li_vrequest_error(liVRequest *vr) {
	vr->state = LI_VRS_ERROR;

	li_stream_reset(vr->coninfo->req);
	li_stream_reset(vr->coninfo->resp);

	li_vrequest_joblist_append(vr);
}

void li_vrequest_backend_error(liVRequest *vr, liBackendError berror) {
	if (vr->state < LI_VRS_READ_CONTENT) {
		vr->action_stack.backend_failed = TRUE;
		vr->action_stack.backend_error = berror;
		li_vrequest_joblist_append(vr);
	} else {
		vr->response.http_status = 503;
		li_vrequest_error(vr);
	}
}

void li_vrequest_backend_overloaded(liVRequest *vr) {
	li_vrequest_backend_error(vr, LI_BACKEND_OVERLOAD);
}
void li_vrequest_backend_dead(liVRequest *vr) {
	li_vrequest_backend_error(vr, LI_BACKEND_DEAD);
}


/* resets fields which weren't reset in favor of keep-alive tracking */
void li_vrequest_start(liVRequest *vr) {
	if (LI_VRS_CLEAN == vr->state) {
		li_request_reset(&vr->request);
	}

	vr->ts_started = CUR_TS(vr->wrk);
}

/* received all request headers */
void li_vrequest_handle_request_headers(liVRequest *vr) {
	if (LI_VRS_CLEAN == vr->state) {
		vr->state = LI_VRS_HANDLE_REQUEST_HEADERS;
	}
	li_vrequest_joblist_append(vr);
}

/* response completely ready */
gboolean li_vrequest_handle_direct(liVRequest *vr) {
	if (li_vrequest_handle_indirect(vr, NULL)) {
		li_vrequest_indirect_connect(vr, li_stream_null_new(&vr->wrk->jobqueue), li_stream_plug_new(&vr->wrk->jobqueue));

		/* release reference from _new */
		li_stream_release(vr->backend_drain);
		li_stream_release(vr->backend_source);

		vr->direct_out = vr->backend_source->out;
		vr->direct_out->is_closed = TRUE;

		li_vrequest_indirect_headers_ready(vr);

		return TRUE;
	} else {
		return FALSE;
	}
}

/* handle request over time */
gboolean li_vrequest_handle_indirect(liVRequest *vr, liPlugin *p) {
	if (vr->state < LI_VRS_READ_CONTENT) {
		vr->state = LI_VRS_READ_CONTENT;
		vr->backend = p;

		return TRUE;
	} else {
		return FALSE;
	}
}

void li_vrequest_indirect_connect(liVRequest *vr, liStream *backend_drain, liStream* backend_source) {
	assert(LI_VRS_READ_CONTENT == vr->state);
	assert(NULL != backend_drain);

	li_stream_acquire(backend_drain);
	vr->backend_drain = backend_drain;

	/* connect in-queue */
	if (NULL != vr->filters_in_last) {
		li_stream_connect(vr->filters_in_last, vr->backend_drain);
		li_stream_connect(vr->coninfo->req, vr->filters_in_first);
	} else {
		/* no filters */
		li_stream_connect(vr->coninfo->req, vr->backend_drain);
	}

	li_stream_acquire(backend_source);
	vr->backend_source = backend_source;

	li_chunkqueue_set_limit(backend_source->out, vr->coninfo->resp->out->limit);

	li_vrequest_joblist_append(vr);
}

/* received all response headers/status code - call once from your indirect handler */
void li_vrequest_indirect_headers_ready(liVRequest* vr) {
	assert(LI_VRS_HANDLE_RESPONSE_HEADERS > vr->state);

	vr->state = LI_VRS_HANDLE_RESPONSE_HEADERS;

	li_vrequest_joblist_append(vr);
}



gboolean li_vrequest_is_handled(liVRequest *vr) {
	return vr->state >= LI_VRS_READ_CONTENT;
}

static liHandlerResult vrequest_do_handle_actions(liVRequest *vr) {
	liHandlerResult res = li_action_execute(vr);
	switch (res) {
	case LI_HANDLER_GO_ON:
		if (vr->state == LI_VRS_HANDLE_REQUEST_HEADERS) {
			/* request not handled */
			li_vrequest_handle_direct(vr);
			if (vr->request.http_method == LI_HTTP_METHOD_OPTIONS) {
				vr->response.http_status = 200;
				li_http_header_append(vr->response.headers, CONST_STR_LEN("Allow"), CONST_STR_LEN("OPTIONS, GET, HEAD, POST"));
			} else {
				vr->response.http_status = 404;
				if (CORE_OPTION(LI_CORE_OPTION_DEBUG_REQUEST_HANDLING).boolean) {
					VR_DEBUG(vr, "%s", "actions didn't handle request");
				}
			}
			return LI_HANDLER_GO_ON;
		}
		/* otherwise state already changed */
		break;
	case LI_HANDLER_COMEBACK:
		li_vrequest_joblist_append(vr); /* come back later */
		return LI_HANDLER_COMEBACK;
	case LI_HANDLER_WAIT_FOR_EVENT:
		return LI_HANDLER_WAIT_FOR_EVENT;
	case LI_HANDLER_ERROR:
		return LI_HANDLER_ERROR;
	}
	return LI_HANDLER_GO_ON;
}

void li_vrequest_state_machine(liVRequest *vr) {
	gboolean done;
	do {
		done = TRUE;

		switch (vr->state) {
		case LI_VRS_CLEAN:
			break;

		case LI_VRS_HANDLE_REQUEST_HEADERS:
			if (CORE_OPTION(LI_CORE_OPTION_DEBUG_REQUEST_HANDLING).boolean) {
				VR_DEBUG(vr, "%s", "handle request header");
			}
			switch (vrequest_do_handle_actions(vr)) {
			case LI_HANDLER_GO_ON:
				break;
			case LI_HANDLER_COMEBACK:
				li_vrequest_joblist_append(vr); /* come back later */
				return;
			case LI_HANDLER_WAIT_FOR_EVENT:
				return;
			case LI_HANDLER_ERROR:
				li_vrequest_error(vr);
				return;
			}

			done = FALSE;

			break;

		case LI_VRS_READ_CONTENT:
			if (CORE_OPTION(LI_CORE_OPTION_DEBUG_REQUEST_HANDLING).boolean) {
				VR_DEBUG(vr, "%s", "read content");
			}
			break;

		case LI_VRS_HANDLE_RESPONSE_HEADERS:
			if (CORE_OPTION(LI_CORE_OPTION_DEBUG_REQUEST_HANDLING).boolean) {
				VR_DEBUG(vr, "%s", "handle response header");
			}
			switch (vrequest_do_handle_actions(vr)) {
			case LI_HANDLER_GO_ON:
				break;
			case LI_HANDLER_COMEBACK:
				li_vrequest_joblist_append(vr); /* come back later */
				return;
			case LI_HANDLER_WAIT_FOR_EVENT:
				return;
			case LI_HANDLER_ERROR:
				li_vrequest_error(vr);
				return;
			}

			if (LI_VRS_HANDLE_RESPONSE_HEADERS != vr->state) break;

			vr->state = LI_VRS_WRITE_CONTENT;

			/* connect out-queue to signal that the headers are ready */
			if (NULL != vr->direct_out) vr->direct_out->is_closed = TRUE; /* make sure this is closed for direct responses */
			if (NULL != vr->filters_out_last) {
				li_stream_connect(vr->backend_source, vr->filters_out_first);
				li_stream_connect(vr->filters_out_last, vr->coninfo->resp);
			} else {
				/* no filters */
				li_stream_connect(vr->backend_source, vr->coninfo->resp);
			}

			done = FALSE;
			break;

		case LI_VRS_WRITE_CONTENT:
			if (CORE_OPTION(LI_CORE_OPTION_DEBUG_REQUEST_HANDLING).boolean) {
				VR_DEBUG(vr, "%s", "write content");
			}
			break;

		case LI_VRS_ERROR:
			if (CORE_OPTION(LI_CORE_OPTION_DEBUG_REQUEST_HANDLING).boolean) {
				VR_DEBUG(vr, "%s", "error");
			}
			vr->coninfo->callbacks->handle_response_error(vr);
			return;
		}
	} while (!done);
}

void li_vrequest_joblist_append(liVRequest *vr) {
	li_job_later(&vr->wrk->jobqueue, &vr->job);
}

liJobRef* li_vrequest_get_ref(liVRequest *vr) {
	return li_job_ref(&vr->wrk->jobqueue, &vr->job);
}

gboolean li_vrequest_redirect(liVRequest *vr, GString *uri) {
	if (!li_vrequest_handle_direct(vr))
		return FALSE;

	vr->response.http_status = 301;
	li_http_header_overwrite(vr->response.headers, CONST_STR_LEN("Location"), GSTR_LEN(uri));

	return TRUE;
}

gboolean li_vrequest_redirect_directory(liVRequest *vr) {
	GString *uri = vr->wrk->tmp_str;

	/* redirect to scheme + host + path + / + querystring if directory without trailing slash */
	/* TODO: local addr if HTTP 1.0 without host header, url encoding */

	if (li_vrequest_is_handled(vr)) return FALSE;

	g_string_truncate(uri, 0);
	g_string_append_len(uri, GSTR_LEN(vr->request.uri.scheme));
	g_string_append_len(uri, CONST_STR_LEN("://"));
	if (vr->request.uri.authority->len > 0) {
		g_string_append_len(uri, GSTR_LEN(vr->request.uri.authority));
	} else {
		g_string_append_len(uri, GSTR_LEN(vr->coninfo->local_addr_str));
	}
	g_string_append_len(uri, GSTR_LEN(vr->request.uri.raw_orig_path));
	g_string_append_c(uri, '/');
	if (vr->request.uri.query->len) {
		g_string_append_c(uri, '?');
		g_string_append_len(uri, GSTR_LEN(vr->request.uri.query));
	}

	return li_vrequest_redirect(vr, uri);
}

static void update_stats_avg(ev_tstamp now, liConInfo *coninfo) {
	if ((now - coninfo->stats.last_avg) >= 5.0) {
		coninfo->stats.bytes_out_5s_diff = coninfo->stats.bytes_out - coninfo->stats.bytes_out_5s;
		coninfo->stats.bytes_out_5s = coninfo->stats.bytes_out;
		coninfo->stats.bytes_in_5s_diff = coninfo->stats.bytes_in - coninfo->stats.bytes_in_5s;
		coninfo->stats.bytes_in_5s = coninfo->stats.bytes_in;
		coninfo->stats.last_avg = now;
	}
}

void li_vrequest_update_stats_in(liVRequest *vr, goffset transferred) {
	liConInfo *coninfo = vr->coninfo;
	vr->wrk->stats.bytes_in += transferred;
	coninfo->stats.bytes_in += transferred;

	update_stats_avg(ev_now(vr->wrk->loop), coninfo);
}

void li_vrequest_update_stats_out(liVRequest *vr, goffset transferred) {
	liConInfo *coninfo = vr->coninfo;
	vr->wrk->stats.bytes_out += transferred;
	coninfo->stats.bytes_out += transferred;

	update_stats_avg(ev_now(vr->wrk->loop), coninfo);
}

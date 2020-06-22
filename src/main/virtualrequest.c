
#include <lighttpd/base.h>
#include <lighttpd/plugin_core.h>
#include <lighttpd/filter_buffer_on_disk.h>

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

	li_action_stack_init(&vr->action_stack);

	li_job_init(&vr->job, vrequest_job_cb);

	vr->stat_cache_entries = g_ptr_array_sized_new(2);

	return vr;
}

void li_vrequest_free(liVRequest* vr) {
	liServer *srv = vr->wrk->srv;

	vr->direct_out = NULL;
	li_stream_safe_reset_and_release(&vr->backend_source);
	li_stream_safe_reset_and_release(&vr->backend_drain);

	li_filter_buffer_on_disk_stop(vr->in_buffer_on_disk_stream);
	li_stream_safe_reset_and_release(&vr->in_buffer_on_disk_stream);
	li_stream_safe_reset_and_release(&vr->wait_for_request_body_stream);

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

	vr->direct_out = NULL;
	li_stream_safe_reset_and_release(&vr->backend_source);
	li_stream_safe_reset_and_release(&vr->backend_drain);

	li_filter_buffer_on_disk_stop(vr->in_buffer_on_disk_stream);
	li_stream_safe_reset_and_release(&vr->in_buffer_on_disk_stream);
	li_stream_safe_reset_and_release(&vr->wait_for_request_body_stream);

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

	vr->ts_started = li_cur_ts(vr->wrk);
}

/* received all request headers */
void li_vrequest_handle_request_headers(liVRequest *vr) {
	if (LI_VRS_CLEAN == vr->state) {
		vr->state = LI_VRS_HANDLE_REQUEST_HEADERS;
	}
	li_vrequest_joblist_append(vr);
}

typedef struct {
	liStream stream;

	liVRequest *vr;
	gboolean have_mem_chunk, ready;
} wait_for_request_body_stream;

static void wait_for_request_body_stream_cb(liStream *stream, liStreamEvent event) {
	wait_for_request_body_stream *ws = LI_CONTAINER_OF(stream, wait_for_request_body_stream, stream);

	switch (event) {
	case LI_STREAM_NEW_DATA:
		if (NULL == ws->stream.source) return;
		if (ws->have_mem_chunk || ws->ready) {
			li_chunkqueue_steal_all(ws->stream.out, ws->stream.source->out);
		} else {
			liChunkQueue *in = ws->stream.source->out, *out = ws->stream.out;
			while (in->length > 0) {
				liChunk *c = li_chunkqueue_first_chunk(in);
				LI_FORCE_ASSERT(NULL != c);
				if (FILE_CHUNK != c->type) {
					ws->have_mem_chunk = TRUE;
					li_chunkqueue_steal_all(out, in);
					break;
				}
				li_chunkqueue_steal_chunk(out, in);
			}
		}
		if (ws->stream.source->out->is_closed) {
			ws->stream.out->is_closed = TRUE;
		}
		if (!ws->ready && (ws->stream.out->is_closed || 
		      (ws->have_mem_chunk && li_chunkqueue_limit_available(ws->stream.out) < 1024))) {
			ws->ready = TRUE;
			if (NULL != ws->vr) li_vrequest_joblist_append(ws->vr);
			ws->vr = NULL;
		}
		li_stream_notify(stream);
		break;
	case LI_STREAM_NEW_CQLIMIT:
		break;
	case LI_STREAM_CONNECTED_DEST:
		ws->ready = TRUE;
		ws->vr = NULL;
		break;
	case LI_STREAM_CONNECTED_SOURCE:
		break;
	case LI_STREAM_DISCONNECTED_DEST:
		if (!ws->stream.out->is_closed || 0 != ws->stream.out->length) {
			li_stream_disconnect(stream);
		}
		break;
	case LI_STREAM_DISCONNECTED_SOURCE:
		if (!ws->stream.out->is_closed) {
			li_stream_disconnect_dest(stream);
			if (!ws->ready) {
				ws->ready = TRUE;
				if (NULL != ws->vr) li_vrequest_joblist_append(ws->vr);
				ws->vr = NULL;
			}
		}
		break;
	case LI_STREAM_DESTROY:
		g_slice_free(wait_for_request_body_stream, ws);
		break;
	}
}

static liStream* wait_for_request_body_stream_new(liVRequest *vr) {
	wait_for_request_body_stream *ws = g_slice_new0(wait_for_request_body_stream);
	ws->vr = vr;
	li_stream_init(&ws->stream, &vr->wrk->loop, wait_for_request_body_stream_cb);
	return &ws->stream;
}

static gboolean wait_for_request_body_stream_ready(liStream *stream) {
	wait_for_request_body_stream *ws;

	if (NULL == stream) return FALSE;
	LI_FORCE_ASSERT(wait_for_request_body_stream_cb == stream->cb);
	ws = LI_CONTAINER_OF(stream, wait_for_request_body_stream, stream);
	return ws->ready;
}

gboolean li_vrequest_wait_for_request_body(liVRequest *vr) {
	goffset lim_avail;

	/* too late to wait? */
	if (vr->state > LI_VRS_HANDLE_REQUEST_HEADERS) return TRUE;
	if (0 == vr->request.content_length) return TRUE;

	if (NULL != vr->wait_for_request_body_stream) {
		if (wait_for_request_body_stream_ready(vr->wait_for_request_body_stream)) return TRUE;
		return FALSE; /* still waiting */
	}

	/* don't start waiting if buffer isn't enabled */
	if (!CORE_OPTION(LI_CORE_OPTION_BUFFER_ON_DISK_REQUEST_BODY).boolean) return TRUE;

	lim_avail = li_chunkqueue_limit_available(vr->coninfo->req->out);

	vr->wait_for_request_body_stream = wait_for_request_body_stream_new(vr);

	if (vr->request.content_length < 0 || lim_avail < 0 || vr->request.content_length > lim_avail) {
		vr->in_buffer_on_disk_stream = li_filter_buffer_on_disk(vr, -1, FALSE);
		li_stream_connect(vr->coninfo->req, vr->in_buffer_on_disk_stream);
		li_stream_connect(vr->in_buffer_on_disk_stream, vr->wait_for_request_body_stream);
	} else {
		li_stream_connect(vr->coninfo->req, vr->wait_for_request_body_stream);
	}

	return FALSE;
}

/* response completely ready */
gboolean li_vrequest_handle_direct(liVRequest *vr) {
	if (li_vrequest_handle_indirect(vr, NULL)) {
		li_vrequest_indirect_connect(vr, li_stream_null_new(&vr->wrk->loop), li_stream_plug_new(&vr->wrk->loop));

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
	liStream *req_in;

	LI_FORCE_ASSERT(LI_VRS_READ_CONTENT == vr->state);
	LI_FORCE_ASSERT(NULL != backend_drain);
	LI_FORCE_ASSERT(NULL != backend_source);

	li_stream_acquire(backend_drain);
	li_stream_acquire(backend_source);

	vr->backend_drain = backend_drain;

	if (NULL != vr->wait_for_request_body_stream) {
		req_in = vr->wait_for_request_body_stream;
		li_filter_buffer_on_disk_stop(vr->in_buffer_on_disk_stream);
	} else {
		req_in = vr->coninfo->req;
	}

	/* connect in-queue */
	if (NULL != vr->filters_in_last) {
		li_stream_connect(vr->filters_in_last, vr->backend_drain);
		li_stream_connect(req_in, vr->filters_in_first);
	} else {
		/* no filters */
		li_stream_connect(req_in, vr->backend_drain);
	}

	vr->backend_source = backend_source;

	li_chunkqueue_set_limit(backend_source->out, vr->coninfo->resp->out->limit);

	li_vrequest_joblist_append(vr);
}

/* received all response headers/status code - call once from your indirect handler */
void li_vrequest_indirect_headers_ready(liVRequest* vr) {
	LI_FORCE_ASSERT(LI_VRS_HANDLE_RESPONSE_HEADERS > vr->state);

	vr->state = LI_VRS_HANDLE_RESPONSE_HEADERS;

	li_vrequest_joblist_append(vr);
}

void li_vrequest_connection_upgrade(liVRequest *vr, liStream *backend_drain, liStream *backend_source) {
	LI_FORCE_ASSERT(LI_VRS_HANDLE_RESPONSE_HEADERS > vr->state);

	/* abort config handling. no filter, no more headers, ... */
	vr->state = LI_VRS_WRITE_CONTENT;
	li_action_stack_reset(vr, &vr->action_stack);

	if (CORE_OPTION(LI_CORE_OPTION_DEBUG_REQUEST_HANDLING).boolean) {
		VR_DEBUG(vr, "%s", "connection uprade");
	}

	/* we don't want these to be disconnected by a li_vrequest_reset */
	li_stream_safe_release(&vr->backend_drain);
	li_stream_safe_release(&vr->backend_source);

	vr->coninfo->callbacks->connection_upgrade(vr, backend_drain, backend_source);
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
			LI_FORCE_ASSERT(li_vrequest_handle_direct(vr));
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
	li_job_later(&vr->wrk->loop.jobqueue, &vr->job);
}

liJobRef* li_vrequest_get_ref(liVRequest *vr) {
	return li_job_ref(&vr->wrk->loop.jobqueue, &vr->job);
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
	g_string_append_len(uri, GSTR_LEN(vr->request.uri.path));
	g_string_append_c(uri, '/');
	if (vr->request.uri.query->len) {
		g_string_append_c(uri, '?');
		g_string_append_len(uri, GSTR_LEN(vr->request.uri.query));
	}

	return li_vrequest_redirect(vr, uri);
}

static void update_stats_avg(li_tstamp now, liConInfo *coninfo) {
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

	update_stats_avg(li_cur_ts(vr->wrk), coninfo);
}

void li_vrequest_update_stats_out(liVRequest *vr, goffset transferred) {
	liConInfo *coninfo = vr->coninfo;
	vr->wrk->stats.bytes_out += transferred;
	coninfo->stats.bytes_out += transferred;

	update_stats_avg(li_cur_ts(vr->wrk), coninfo);
}

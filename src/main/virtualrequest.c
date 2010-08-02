
#include <lighttpd/base.h>
#include <lighttpd/plugin_core.h>

static void filters_init(liFilters *fs) {
	fs->queue = g_ptr_array_new();
	fs->in = li_chunkqueue_new();
	fs->out = li_chunkqueue_new();
}

static void filters_clean(liVRequest *vr, liFilters *fs) {
	guint i;
	for (i = 0; i < fs->queue->len; i++) {
		liFilter *f = (liFilter*) g_ptr_array_index(fs->queue, i);
		if (f->handle_free && f->param) f->handle_free(vr, f);
		if (i > 0) li_chunkqueue_free(fs->in);
		g_slice_free(liFilter, f);
	}
	g_ptr_array_free(fs->queue, TRUE);
	li_chunkqueue_free(fs->in);
	li_chunkqueue_free(fs->out);
}

static void filters_reset(liVRequest *vr, liFilters *fs) {
	guint i;
	for (i = 0; i < fs->queue->len; i++) {
		liFilter *f = (liFilter*) g_ptr_array_index(fs->queue, i);
		if (f->handle_free && f->param) f->handle_free(vr, f);
		if (i > 0) li_chunkqueue_free(f->in);
		g_slice_free(liFilter, f);
	}
	g_ptr_array_set_size(fs->queue, 0);
	li_chunkqueue_reset(fs->in);
	li_chunkqueue_reset(fs->out);
}

static gboolean filters_handle_out_close(liVRequest *vr, liFilters *fs) {
	guint i;
	if (0 == fs->queue->len) {
		if (fs->out->is_closed) fs->in->is_closed = TRUE;
		return TRUE;
	}
	for (i = fs->queue->len; i-- > 0; ) {
		liFilter *f = (liFilter*) g_ptr_array_index(fs->queue, i);
		if (f->out->is_closed && !f->knows_out_is_closed) {
			f->knows_out_is_closed = TRUE;
			switch (f->handle_data(vr, f)) {
			case LI_HANDLER_GO_ON:
				break;
			case LI_HANDLER_COMEBACK:
				li_vrequest_joblist_append(vr);
				break;
			case LI_HANDLER_WAIT_FOR_EVENT:
				break; /* ignore - filter has to call li_vrequest_joblist_append(vr); */
			case LI_HANDLER_ERROR:
				if (CORE_OPTION(LI_CORE_OPTION_DEBUG_REQUEST_HANDLING).boolean) {
					VR_DEBUG(vr, "filter %i return an error", i);
				}
				return FALSE;
			}
		}
	}
	return TRUE;
}

static gboolean filters_run(liVRequest *vr, liFilters *fs) {
	guint i;
	if (0 == fs->queue->len) {
		li_chunkqueue_steal_all(fs->out, fs->in);
		if (fs->in->is_closed) fs->out->is_closed = TRUE;
		return TRUE;
	}
	for (i = 0; i < fs->queue->len; i++) {
		liFilter *f = (liFilter*) g_ptr_array_index(fs->queue, i);
		switch (f->handle_data(vr, f)) {
		case LI_HANDLER_GO_ON:
			break;
		case LI_HANDLER_COMEBACK:
			li_vrequest_joblist_append(vr);
			break;
		case LI_HANDLER_WAIT_FOR_EVENT:
			break; /* ignore - filter has to call li_vrequest_joblist_append(vr); */
		case LI_HANDLER_ERROR:
			if (CORE_OPTION(LI_CORE_OPTION_DEBUG_REQUEST_HANDLING).boolean) {
				VR_DEBUG(vr, "filter %i return an error", i);
			}
			return FALSE;
		}
		f->knows_out_is_closed = f->out->is_closed;
		if (f->in->is_closed && i > 0) {
			guint j;
			for (j = i; j-- > 0; ) {
				liFilter *g = (liFilter*) g_ptr_array_index(fs->queue, j);
				if (g->knows_out_is_closed) break;
				g->knows_out_is_closed = TRUE;
				switch (f->handle_data(vr, f)) {
				case LI_HANDLER_GO_ON:
					break;
				case LI_HANDLER_COMEBACK:
					li_vrequest_joblist_append(vr);
					break;
				case LI_HANDLER_WAIT_FOR_EVENT:
					break; /* ignore - filter has to call li_vrequest_joblist_append(vr); */
				case LI_HANDLER_ERROR:
					return FALSE;
				}
				if (!g->in->is_closed) break;
			}
		}
	}
	return TRUE;
}

static liFilter* filters_add(liFilters *fs, liFilterHandlerCB handle_data, liFilterFreeCB handle_free, gpointer param) {
	liFilter *f = g_slice_new0(liFilter);
	f->out = fs->out;
	f->param = param;
	f->handle_data = handle_data;
	f->handle_free = handle_free;
	if (0 == fs->queue->len) {
		f->in = fs->in;
	} else {
		liFilter *prev = (liFilter*) g_ptr_array_index(fs->queue, fs->queue->len - 1);
		f->in = prev->out = li_chunkqueue_new();
		li_chunkqueue_set_limit(f->in, fs->in->limit);
	}
	g_ptr_array_add(fs->queue, f);
	return f;
}

liFilter* li_vrequest_add_filter_in(liVRequest *vr, liFilterHandlerCB handle_data, liFilterFreeCB handle_free, gpointer param) {
	return filters_add(&vr->filters_in, handle_data, handle_free, param);
}

liFilter* li_vrequest_add_filter_out(liVRequest *vr, liFilterHandlerCB handle_data, liFilterFreeCB handle_free, gpointer param) {
	return filters_add(&vr->filters_out, handle_data, handle_free, param);
}

liVRequest* li_vrequest_new(liConnection *con, liConInfo *coninfo) {
	liServer *srv = con->srv;
	liVRequest *vr = g_slice_new0(liVRequest);

	vr->coninfo = coninfo;
	vr->wrk = con->wrk;
	vr->ref = g_slice_new0(liVRequestRef);
	vr->ref->refcount = 1;
	vr->ref->vr = vr;
	vr->ref->wrk = con->wrk;
	vr->state = LI_VRS_CLEAN;

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

	filters_init(&vr->filters_in);
	filters_init(&vr->filters_out);
	vr->vr_in = vr->filters_in.in;
	vr->in_memory = vr->filters_in.out;
	vr->in = li_chunkqueue_new();
	vr->out = vr->filters_out.in;
	vr->vr_out = vr->filters_out.out;

	li_chunkqueue_use_limit(vr->in, vr);
	li_chunkqueue_set_limit(vr->vr_in, vr->in->limit);
	li_chunkqueue_set_limit(vr->in_memory, vr->in->limit);
	li_chunkqueue_use_limit(vr->out, vr);
	li_chunkqueue_set_limit(vr->vr_out, vr->out->limit);

	vr->in_buffer_state.flush_limit = -1; /* wait until upload is complete */
	vr->in_buffer_state.split_on_file_chunks = FALSE;

	vr->stat_cache_entries = g_ptr_array_sized_new(2);

	vr->job_queue_link.data = vr;

	li_action_stack_init(&vr->action_stack);

	vr->throttle.wqueue_elem.data = vr;
	vr->throttle.pool.lnk.data = vr;

	return vr;
}

void li_vrequest_free(liVRequest* vr) {
	liServer *srv = vr->wrk->srv;

	li_action_stack_clear(vr, &vr->action_stack);
	if (vr->state != LI_VRS_CLEAN) {
		li_plugins_handle_vrclose(vr);
	}
	g_ptr_array_free(vr->plugin_ctx, TRUE);

	li_request_clear(&vr->request);
	li_physical_clear(&vr->physical);
	li_response_clear(&vr->response);
	li_environment_clear(&vr->env);

	filters_clean(vr, &vr->filters_in);
	filters_clean(vr, &vr->filters_out);
	li_chunkqueue_free(vr->in);
	li_filter_buffer_on_disk_reset(&vr->in_buffer_state);

	if (g_atomic_int_get(&vr->queued)) { /* atomic access shouldn't be needed here; no one else can access vr here... */
		g_queue_unlink(&vr->wrk->job_queue, &vr->job_queue_link);
		g_atomic_int_set(&vr->queued, 0);
	}

	g_slice_free1(srv->option_def_values->len * sizeof(liOptionValue), vr->options);
	{
		guint i;
		for (i = 0; i < srv->optionptr_def_values->len; i++) {
			li_release_optionptr(srv, vr->optionptrs[i]);
		}
	}
	g_slice_free1(srv->optionptr_def_values->len * sizeof(liOptionPtrValue*), vr->optionptrs);


	while (vr->stat_cache_entries->len > 0 ) {
		liStatCacheEntry *sce = g_ptr_array_index(vr->stat_cache_entries, 0);
		li_stat_cache_entry_release(vr, sce);
	}
	g_ptr_array_free(vr->stat_cache_entries, TRUE);

	vr->ref->vr = NULL;
	if (g_atomic_int_dec_and_test(&vr->ref->refcount)) {
		g_slice_free(liVRequestRef, vr->ref);
	}

	g_slice_free(liVRequest, vr);
}

void li_vrequest_reset(liVRequest *vr, gboolean keepalive) {
	liServer *srv = vr->wrk->srv;

	li_action_stack_reset(vr, &vr->action_stack);
	if (vr->state != LI_VRS_CLEAN) {
		li_plugins_handle_vrclose(vr);
	}
	{
		gint len = vr->plugin_ctx->len;
		g_ptr_array_set_size(vr->plugin_ctx, 0);
		g_ptr_array_set_size(vr->plugin_ctx, len);
	}

	vr->state = LI_VRS_CLEAN;

	vr->backend = NULL;

	/* don't reset request for keep-alive tracking */
	if (!keepalive) li_request_reset(&vr->request);
	li_physical_reset(&vr->physical);
	li_response_reset(&vr->response);
	li_environment_reset(&vr->env);

	filters_reset(vr, &vr->filters_in);
	filters_reset(vr, &vr->filters_out);
	li_chunkqueue_reset(vr->in);
	li_filter_buffer_on_disk_reset(&vr->in_buffer_state);
	vr->in_buffer_state.flush_limit = -1; /* wait until upload is complete */
	vr->in_buffer_state.split_on_file_chunks = FALSE;

	li_chunkqueue_use_limit(vr->in, vr);
	li_chunkqueue_set_limit(vr->vr_in, vr->in->limit);
	li_chunkqueue_set_limit(vr->in_memory, vr->in->limit);
	li_chunkqueue_use_limit(vr->out, vr);
	li_chunkqueue_set_limit(vr->vr_out, vr->out->limit);

	if (g_atomic_int_get(&vr->queued)) { /* atomic access shouldn't be needed here; no one else can access vr here... */
		g_queue_unlink(&vr->wrk->job_queue, &vr->job_queue_link);
		g_atomic_int_set(&vr->queued, 0);
	}

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

	if (1 != g_atomic_int_get(&vr->ref->refcount)) {
		/* If we are not the only user of vr->ref we have to get a new one and detach the old */
		vr->ref->vr = NULL;
		if (g_atomic_int_dec_and_test(&vr->ref->refcount)) {
			g_slice_free(liVRequestRef, vr->ref);
		}
		vr->ref = g_slice_new0(liVRequestRef);
		vr->ref->refcount = 1;
		vr->ref->vr = vr;
		vr->ref->wrk = vr->wrk;
	}
}

liVRequestRef* li_vrequest_get_ref(liVRequest *vr) {
	liVRequestRef* vr_ref = vr->ref;
	g_assert(vr_ref->refcount > 0);
	g_atomic_int_inc(&vr_ref->refcount);
	return vr_ref;
}

void li_vrequest_ref_acquire(liVRequestRef *vr_ref) {
	g_assert(vr_ref->refcount > 0);
	g_atomic_int_inc(&vr_ref->refcount);
}

liVRequest* li_vrequest_ref_release(liVRequestRef *vr_ref) {
	liVRequest *vr = vr_ref->vr;
	g_assert(vr_ref->refcount > 0);
	if (g_atomic_int_dec_and_test(&vr_ref->refcount)) {
		g_assert(vr == NULL); /* we are the last user, and the ref holded by vr itself is handled extra, so the vr was already reset */
		g_slice_free(liVRequestRef, vr_ref);
	}
	return vr;
}

void li_vrequest_error(liVRequest *vr) {
	vr->state = LI_VRS_ERROR;
	vr->out->is_closed = TRUE;
	li_vrequest_joblist_append(vr);
}

void li_vrequest_backend_error(liVRequest *vr, liBackendError berror) {
	vr->action_stack.backend_failed = TRUE;
	vr->action_stack.backend_error = berror;
	vr->state = LI_VRS_HANDLE_REQUEST_HEADERS;
	vr->backend = NULL;
	li_vrequest_joblist_append(vr);
}

void li_vrequest_backend_overloaded(liVRequest *vr) {
	li_vrequest_backend_error(vr, BACKEND_OVERLOAD);
}
void li_vrequest_backend_dead(liVRequest *vr) {
	li_vrequest_backend_error(vr, BACKEND_DEAD);
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

/* received (partial) request content */
void li_vrequest_handle_request_body(liVRequest *vr) {
	if (LI_VRS_READ_CONTENT <= vr->state) {
		li_vrequest_joblist_append(vr);
	}
}

/* received all response headers/status code - call once from your indirect handler */
void li_vrequest_handle_response_headers(liVRequest *vr) {
	if (LI_VRS_HANDLE_RESPONSE_HEADERS > vr->state) {
		vr->state = LI_VRS_HANDLE_RESPONSE_HEADERS;
	}
	li_vrequest_joblist_append(vr);
}

/* received (partial) response content - call from your indirect handler */
void li_vrequest_handle_response_body(liVRequest *vr) {
	if (LI_VRS_WRITE_CONTENT == vr->state) {
		li_vrequest_joblist_append(vr);
	}
}

/* response completely ready */
gboolean li_vrequest_handle_direct(liVRequest *vr) {
	if (vr->state < LI_VRS_READ_CONTENT) {
		vr->state = LI_VRS_HANDLE_RESPONSE_HEADERS;
		vr->out->is_closed = TRUE;
		vr->backend = NULL;
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
		li_vrequest_error(vr);
		return LI_HANDLER_ERROR;
	}
	return LI_HANDLER_GO_ON;
}


static gboolean vrequest_do_handle_read(liVRequest *vr) {
	if (vr->backend && vr->backend->handle_request_body) {
		goffset lim_avail;

		if (vr->in->is_closed) vr->in_memory->is_closed = TRUE;
		if (!filters_handle_out_close(vr, &vr->filters_in)) {
			li_vrequest_error(vr);
		}
		if (!filters_run(vr, &vr->filters_in)) {
			li_vrequest_error(vr);
		}

		if (vr->in_buffer_state.tempfile || vr->request.content_length < 0 || vr->request.content_length > 64*1024 ||
			((lim_avail = li_chunkqueue_limit_available(vr->in)) <= 32*1024 && lim_avail >= 0)) {
			switch (li_filter_buffer_on_disk(vr, vr->in, vr->in_memory, &vr->in_buffer_state)) {
			case LI_HANDLER_GO_ON:
				break;
			case LI_HANDLER_COMEBACK:
				li_vrequest_joblist_append(vr); /* come back later */
				return FALSE;
			case LI_HANDLER_WAIT_FOR_EVENT:
				return FALSE;
			case LI_HANDLER_ERROR:
				li_vrequest_error(vr);
				break;
			}
		} else {
			li_chunkqueue_steal_all(vr->in, vr->in_memory);
			if (vr->in_memory->is_closed) vr->in->is_closed = TRUE;
		}

		switch (vr->backend->handle_request_body(vr, vr->backend)) {
		case LI_HANDLER_GO_ON:
			break;
		case LI_HANDLER_COMEBACK:
			li_vrequest_joblist_append(vr); /* come back later */
			return FALSE;
		case LI_HANDLER_WAIT_FOR_EVENT:
			return FALSE;
		case LI_HANDLER_ERROR:
			li_vrequest_error(vr);
			break;
		}
	} else {
		li_chunkqueue_skip_all(vr->vr_in);
		if (vr->vr_in->is_closed) vr->in->is_closed = TRUE;
	}
	return TRUE;
}

static void vrequest_do_handle_write(liVRequest *vr) {
	if (!filters_handle_out_close(vr, &vr->filters_out)) {
		li_vrequest_error(vr);
		return;
	}
	if (!filters_run(vr, &vr->filters_out)) {
		li_vrequest_error(vr);
		return;
	}

	switch (vr->coninfo->callbacks->handle_response_body(vr)) {
	case LI_HANDLER_GO_ON:
		break;
	case LI_HANDLER_COMEBACK:
		li_vrequest_joblist_append(vr); /* come back later */
		return;
	case LI_HANDLER_WAIT_FOR_EVENT:
		return;
	case LI_HANDLER_ERROR:
		li_vrequest_error(vr);
		break;
	}
	return;
}

void li_vrequest_state_machine(liVRequest *vr) {
	gboolean done = FALSE;
	liHandlerResult res;
	do {
		switch (vr->state) {
		case LI_VRS_CLEAN:
			done = TRUE;
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
				if (vr->state == LI_VRS_HANDLE_REQUEST_HEADERS) return;
				break; /* go on to get post data/response headers if request is already handled */
			case LI_HANDLER_ERROR:
				return;
			}
			res = vr->coninfo->callbacks->handle_request_headers(vr);
			switch (res) {
			case LI_HANDLER_GO_ON:
				if (vr->state == LI_VRS_HANDLE_REQUEST_HEADERS) {
					if (vr->request.http_method == LI_HTTP_METHOD_OPTIONS) {
						vr->response.http_status = 200;
						li_http_header_append(vr->response.headers, CONST_STR_LEN("Allow"), CONST_STR_LEN("OPTIONS, GET, HEAD, POST"));
					} else {
						/* unhandled request */
						vr->response.http_status = 404;
					}
					li_vrequest_handle_direct(vr);
				}
				break;
			case LI_HANDLER_COMEBACK:
				li_vrequest_joblist_append(vr); /* come back later */
				done = TRUE;
				break;
			case LI_HANDLER_WAIT_FOR_EVENT:
				done = (vr->state == LI_VRS_HANDLE_REQUEST_HEADERS);
				break;
			case LI_HANDLER_ERROR:
				li_vrequest_error(vr);
				break;
			}
			break;

		case LI_VRS_READ_CONTENT:
			if (CORE_OPTION(LI_CORE_OPTION_DEBUG_REQUEST_HANDLING).boolean) {
				VR_DEBUG(vr, "%s", "read content");
			}
			done = !vrequest_do_handle_read(vr);
			done = done || (vr->state == LI_VRS_READ_CONTENT);
			break;

		case LI_VRS_HANDLE_RESPONSE_HEADERS:
			if (CORE_OPTION(LI_CORE_OPTION_DEBUG_REQUEST_HANDLING).boolean) {
				VR_DEBUG(vr, "%s", "handle response header");
			}
			switch (vrequest_do_handle_actions(vr)) {
			case LI_HANDLER_GO_ON:
				break;
			case LI_HANDLER_COMEBACK:
				return;
			case LI_HANDLER_WAIT_FOR_EVENT:
				return; /* wait to handle response headers */
			case LI_HANDLER_ERROR:
				return;
			}
			res = vr->coninfo->callbacks->handle_response_headers(vr);
			switch (res) {
			case LI_HANDLER_GO_ON:
				vr->state = LI_VRS_WRITE_CONTENT;
				break;
			case LI_HANDLER_COMEBACK:
				li_vrequest_joblist_append(vr); /* come back later */
				done = TRUE;
				break;
			case LI_HANDLER_WAIT_FOR_EVENT:
				done = (vr->state == LI_VRS_HANDLE_REQUEST_HEADERS);
				break;
			case LI_HANDLER_ERROR:
				li_vrequest_error(vr);
				break;
			}
			break;

		case LI_VRS_WRITE_CONTENT:
			if (CORE_OPTION(LI_CORE_OPTION_DEBUG_REQUEST_HANDLING).boolean) {
				VR_DEBUG(vr, "%s", "write content");
			}
			vrequest_do_handle_read(vr);
			vrequest_do_handle_write(vr);
			done = TRUE;
			break;

		case LI_VRS_ERROR:
			if (CORE_OPTION(LI_CORE_OPTION_DEBUG_REQUEST_HANDLING).boolean) {
				VR_DEBUG(vr, "%s", "error");
			}
			/* this will probably reset the vrequest, so stop handling after it */
			vr->coninfo->callbacks->handle_response_error(vr);
			return;
		}
	} while (!done);
}

void li_vrequest_joblist_append(liVRequest *vr) {
	liWorker *wrk = vr->wrk;
	GQueue *const q = &wrk->job_queue;
	if (!g_atomic_int_compare_and_exchange(&vr->queued, 0, 1)) return; /* already in queue */
	g_queue_push_tail_link(q, &vr->job_queue_link);
}

void li_vrequest_joblist_append_async(liVRequestRef *vr_ref) {
	liWorker *wrk = vr_ref->wrk;
	GAsyncQueue *const q = wrk->job_async_queue;
	li_vrequest_ref_acquire(vr_ref);
	g_async_queue_push(q, vr_ref);
	ev_async_send(wrk->loop, &wrk->job_async_queue_watcher);
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

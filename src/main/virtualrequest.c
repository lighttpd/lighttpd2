
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
	fs->skip_ndx = 0;
	for (i = 0; i < fs->queue->len; i++) {
		liFilter *f = (liFilter*) g_ptr_array_index(fs->queue, i);
		if (f->handle_free && f->param) f->handle_free(vr, f);
		if (i > 0) li_chunkqueue_free(fs->in);
		g_slice_free(liFilter, f);
	}
	g_ptr_array_set_size(fs->queue, 0);
	li_chunkqueue_reset(fs->in);
	li_chunkqueue_reset(fs->out);
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
			return FALSE;
		}
	}
	if (fs->out->is_closed) {
		liFilter *f = (liFilter*) g_ptr_array_index(fs->queue, fs->queue->len - 1);
		f->in->is_closed = TRUE;
	}
	for (i = fs->queue->len; i-- > fs->skip_ndx; ) {
		liFilter *f = (liFilter*) g_ptr_array_index(fs->queue, i);
		if (f->in->is_closed) {
			guint j = i;
			while (j-- > fs->skip_ndx) {
				liFilter *ff = (liFilter*) g_ptr_array_index(fs->queue, j);
				ff->in->is_closed = TRUE;
			}
			fs->skip_ndx = i;
		}
	}
	return TRUE;
}

static void filters_add(liFilters *fs, liFilterHandlerCB handle_data, liFilterFreeCB handle_free, gpointer param) {
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
}

void li_vrequest_add_filter_in(liVRequest *vr, liFilterHandlerCB handle_data, liFilterFreeCB handle_free, gpointer param) {
	filters_add(&vr->filters_in, handle_data, handle_free, param);
}

void li_vrequest_add_filter_out(liVRequest *vr, liFilterHandlerCB handle_data, liFilterFreeCB handle_free, gpointer param) {
	filters_add(&vr->filters_out, handle_data, handle_free, param);
}

liVRequest* li_vrequest_new(liConnection *con, liVRequestHandlerCB handle_response_headers, liVRequestHandlerCB handle_response_body, liVRequestHandlerCB handle_response_error, liVRequestHandlerCB handle_request_headers) {
	liServer *srv = con->srv;
	liVRequest *vr = g_slice_new0(liVRequest);

	vr->con = con;
	vr->wrk = con->wrk;
	vr->ref = g_slice_new0(liVRequestRef);
	vr->ref->refcount = 1;
	vr->ref->vr = vr;
	vr->state = LI_VRS_CLEAN;

	vr->handle_response_headers = handle_response_headers;
	vr->handle_response_body = handle_response_body;
	vr->handle_response_error = handle_response_error;
	vr->handle_request_headers = handle_request_headers;

	vr->plugin_ctx = g_ptr_array_new();
	g_ptr_array_set_size(vr->plugin_ctx, g_hash_table_size(srv->plugins));
	vr->options = g_slice_copy(srv->option_def_values->len * sizeof(liOptionValue), srv->option_def_values->data);

	li_request_init(&vr->request);
	li_physical_init(&vr->physical);
	li_response_init(&vr->response);
	li_environment_init(&vr->env);

	filters_init(&vr->filters_in);
	filters_init(&vr->filters_out);
	vr->vr_in = vr->filters_in.in;
	vr->in = vr->filters_in.out;
	vr->out = vr->filters_out.in;
	vr->vr_out = vr->filters_out.out;

	vr->stat_cache_entries = g_ptr_array_sized_new(2);

	vr->job_queue_link.data = vr;

	li_action_stack_init(&vr->action_stack);

	return vr;
}

void li_vrequest_free(liVRequest* vr) {
	guint i;

	li_action_stack_clear(vr, &vr->action_stack);
	li_plugins_handle_vrclose(vr);
	g_ptr_array_free(vr->plugin_ctx, TRUE);

	li_request_clear(&vr->request);
	li_physical_clear(&vr->physical);
	li_response_clear(&vr->response);
	li_environment_clear(&vr->env);

	filters_clean(vr, &vr->filters_in);
	filters_clean(vr, &vr->filters_out);

	if (g_atomic_int_get(&vr->queued)) { /* atomic access shouldn't be needed here; no one else can access vr here... */
		g_queue_unlink(&vr->wrk->job_queue, &vr->job_queue_link);
		g_atomic_int_set(&vr->queued, 0);
	}

	g_slice_free1(vr->wrk->srv->option_def_values->len * sizeof(liOptionValue), vr->options);


	for (i = 0; i < vr->stat_cache_entries->len; i++) {
		liStatCacheEntry *sce = g_ptr_array_index(vr->stat_cache_entries, i);
		li_stat_cache_entry_release(vr, sce);
	}
	g_ptr_array_free(vr->stat_cache_entries, TRUE);

	vr->ref->vr = NULL;
	if (g_atomic_int_dec_and_test(&vr->ref->refcount)) {
		g_slice_free(liVRequestRef, vr->ref);
	}

	g_slice_free(liVRequest, vr);
}

void li_vrequest_reset(liVRequest *vr) {
	guint i;

	li_action_stack_reset(vr, &vr->action_stack);
	li_plugins_handle_vrclose(vr);
	{
		gint len = vr->plugin_ctx->len;
		g_ptr_array_set_size(vr->plugin_ctx, 0);
		g_ptr_array_set_size(vr->plugin_ctx, len);
	}

	vr->state = LI_VRS_CLEAN;

	vr->backend = NULL;

	li_request_reset(&vr->request);
	li_physical_reset(&vr->physical);
	li_response_reset(&vr->response);
	li_environment_reset(&vr->env);

	filters_reset(vr, &vr->filters_in);
	filters_reset(vr, &vr->filters_out);

	if (g_atomic_int_get(&vr->queued)) { /* atomic access shouldn't be needed here; no one else can access vr here... */
		g_queue_unlink(&vr->wrk->job_queue, &vr->job_queue_link);
		g_atomic_int_set(&vr->queued, 0);
	}

	for (i = 0; i < vr->stat_cache_entries->len; i++) {
		liStatCacheEntry *sce = g_ptr_array_index(vr->stat_cache_entries, i);
		li_stat_cache_entry_release(vr, sce);
	}

	memcpy(vr->options, vr->wrk->srv->option_def_values->data, vr->wrk->srv->option_def_values->len * sizeof(liOptionValue));

	if (1 != g_atomic_int_get(&vr->ref->refcount)) {
		/* If we are not the only user of vr->ref we have to get a new one and detach the old */
		vr->ref->vr = NULL;
		if (g_atomic_int_dec_and_test(&vr->ref->refcount)) {
			g_slice_free(liVRequestRef, vr->ref);
		}
		vr->ref = g_slice_new0(liVRequestRef);
		vr->ref->refcount = 1;
		vr->ref->vr = vr;
	}
}

liVRequestRef* li_vrequest_acquire_ref(liVRequest *vr) {
	liVRequestRef* vr_ref = vr->ref;
	g_assert(vr_ref->refcount > 0);
	g_atomic_int_inc(&vr_ref->refcount);
	return vr_ref;
}

liVRequest* li_vrequest_release_ref(liVRequestRef *vr_ref) {
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

static gboolean vrequest_do_handle_actions(liVRequest *vr) {
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
			return TRUE;
		}
		/* otherwise state already changed */
		break;
	case LI_HANDLER_COMEBACK:
		li_vrequest_joblist_append(vr); /* come back later */
		return FALSE;
	case LI_HANDLER_WAIT_FOR_EVENT:
		return FALSE;
	case LI_HANDLER_ERROR:
		li_vrequest_error(vr);
		return FALSE;
	}
	return TRUE;
}


static gboolean vrequest_do_handle_read(liVRequest *vr) {
	if (vr->backend && vr->backend->handle_request_body) {
		if (!filters_run(vr, &vr->filters_in)) {
			li_vrequest_error(vr);
		}

		if (vr->vr_in->is_closed) vr->in->is_closed = TRUE;
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

static gboolean vrequest_do_handle_write(liVRequest *vr) {
	if (!filters_run(vr, &vr->filters_out)) {
		li_vrequest_error(vr);
	}

	switch (vr->handle_response_body(vr)) {
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
	return TRUE;
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
			if (!vrequest_do_handle_actions(vr)) return;
			res = vr->handle_request_headers(vr);
			switch (res) {
			case LI_HANDLER_GO_ON:
				if (vr->state == LI_VRS_HANDLE_REQUEST_HEADERS) {
					/* unhandled request */
					vr->response.http_status = 404;
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
			done = !vrequest_do_handle_read(vr);
			done = done || (vr->state == LI_VRS_READ_CONTENT);
			break;

		case LI_VRS_HANDLE_RESPONSE_HEADERS:
			if (!vrequest_do_handle_actions(vr)) return;
			res = vr->handle_response_headers(vr);
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
			vrequest_do_handle_read(vr);
			vrequest_do_handle_write(vr);
			done = TRUE;
			break;

		case LI_VRS_ERROR:
			/* this will probably reset the vrequest, so stop handling after it */
			vr->handle_response_error(vr);
			return;
		}
	} while (!done);
}

void li_vrequest_joblist_append(liVRequest *vr) {
	liWorker *wrk = vr->wrk;
	GQueue *const q = &wrk->job_queue;
	if (!g_atomic_int_compare_and_exchange(&vr->queued, 0, 1)) return; /* already in queue */
	g_queue_push_tail_link(q, &vr->job_queue_link);
	ev_timer_start(wrk->loop, &wrk->job_queue_watcher);
}

void li_vrequest_joblist_append_async(liVRequest *vr) {
	liWorker *wrk = vr->wrk;
	GAsyncQueue *const q = wrk->job_async_queue;
	if (!g_atomic_int_compare_and_exchange(&vr->queued, 0, 1)) return; /* already in queue */
	g_async_queue_push(q, li_vrequest_acquire_ref(vr));
	ev_async_send(wrk->loop, &wrk->job_async_queue_watcher);
}

gboolean li_vrequest_redirect(liVRequest *vr, GString *uri) {
	if (!li_vrequest_handle_direct(vr))
		return FALSE;

	vr->response.http_status = 301;
	li_http_header_overwrite(vr->response.headers, CONST_STR_LEN("Location"), GSTR_LEN(uri));

	return TRUE;
}

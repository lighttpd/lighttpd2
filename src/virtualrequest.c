
#include <lighttpd/base.h>
#include <lighttpd/plugin_core.h>

static void filters_init(filters *fs) {
	fs->queue = g_ptr_array_new();
	fs->in = chunkqueue_new();
	fs->out = chunkqueue_new();
}

static void filters_clean(vrequest *vr, filters *fs) {
	guint i;
	for (i = 0; i < fs->queue->len; i++) {
		filter *f = (filter*) g_ptr_array_index(fs->queue, i);
		if (f->handle_free && f->param) f->handle_free(vr, f);
		if (i > 0) chunkqueue_free(fs->in);
		g_slice_free(filter, f);
	}
	g_ptr_array_free(fs->queue, TRUE);
	chunkqueue_free(fs->in);
	chunkqueue_free(fs->out);
}

static void filters_reset(vrequest *vr, filters *fs) {
	guint i;
	fs->skip_ndx = 0;
	for (i = 0; i < fs->queue->len; i++) {
		filter *f = (filter*) g_ptr_array_index(fs->queue, i);
		if (f->handle_free && f->param) f->handle_free(vr, f);
		if (i > 0) chunkqueue_free(fs->in);
		g_slice_free(filter, f);
	}
	g_ptr_array_set_size(fs->queue, 0);
	chunkqueue_reset(fs->in);
	chunkqueue_reset(fs->out);
}

static gboolean filters_run(vrequest *vr, filters *fs) {
	guint i;
	if (0 == fs->queue->len) {
		chunkqueue_steal_all(fs->out, fs->in);
		if (fs->in->is_closed) fs->out->is_closed = TRUE;
		return TRUE;
	}
	for (i = 0; i < fs->queue->len; i++) {
		filter *f = (filter*) g_ptr_array_index(fs->queue, i);
		switch (f->handle_data(vr, f)) {
		case HANDLER_GO_ON:
			break;
		case HANDLER_COMEBACK:
			vrequest_joblist_append(vr);
			break;
		case HANDLER_WAIT_FOR_EVENT:
			break; /* ignore - filter has to call vrequest_joblist_append(vr); */
		case HANDLER_ERROR:
			return FALSE;
		}
	}
	if (fs->out->is_closed) {
		filter *f = (filter*) g_ptr_array_index(fs->queue, fs->queue->len - 1);
		f->in->is_closed = TRUE;
	}
	for (i = fs->queue->len; i-- > fs->skip_ndx; ) {
		filter *f = (filter*) g_ptr_array_index(fs->queue, i);
		if (f->in->is_closed) {
			guint j = i;
			while (j-- > fs->skip_ndx) {
				filter *ff = (filter*) g_ptr_array_index(fs->queue, j);
				ff->in->is_closed = TRUE;
			}
			fs->skip_ndx = i;
		}
	}
	return TRUE;
}

static void filters_add(filters *fs, filter_handler handle_data, filter_free handle_free, gpointer param) {
	filter *f = g_slice_new0(filter);
	f->out = fs->out;
	f->param = param;
	f->handle_data = handle_data;
	f->handle_free = handle_free;
	if (0 == fs->queue->len) {
		f->in = fs->in;
	} else {
		filter *prev = (filter*) g_ptr_array_index(fs->queue, fs->queue->len - 1);
		f->in = prev->out = chunkqueue_new();
		chunkqueue_set_limit(f->in, fs->in->limit);
	}
	g_ptr_array_add(fs->queue, f);
}

void vrequest_add_filter_in(vrequest *vr, filter_handler handle_data, filter_free handle_free, gpointer param) {
	filters_add(&vr->filters_in, handle_data, handle_free, param);
}

void vrequest_add_filter_out(vrequest *vr, filter_handler handle_data, filter_free handle_free, gpointer param) {
	filters_add(&vr->filters_out, handle_data, handle_free, param);
}

vrequest* vrequest_new(connection *con, vrequest_handler handle_response_headers, vrequest_handler handle_response_body, vrequest_handler handle_response_error, vrequest_handler handle_request_headers) {
	server *srv = con->srv;
	vrequest *vr = g_slice_new0(vrequest);

	vr->con = con;
	vr->wrk = con->wrk;
	vr->ref = g_slice_new0(vrequest_ref);
	vr->ref->refcount = 1;
	vr->ref->vr = vr;
	vr->state = VRS_CLEAN;

	vr->handle_response_headers = handle_response_headers;
	vr->handle_response_body = handle_response_body;
	vr->handle_response_error = handle_response_error;
	vr->handle_request_headers = handle_request_headers;

	vr->plugin_ctx = g_ptr_array_new();
	g_ptr_array_set_size(vr->plugin_ctx, g_hash_table_size(srv->plugins));
	vr->options = g_slice_copy(srv->option_def_values->len * sizeof(option_value), srv->option_def_values->data);

	request_init(&vr->request);
	physical_init(&vr->physical);
	response_init(&vr->response);
	environment_init(&vr->env);

	filters_init(&vr->filters_in);
	filters_init(&vr->filters_out);
	vr->vr_in = vr->filters_in.in;
	vr->in = vr->filters_in.out;
	vr->out = vr->filters_out.in;
	vr->vr_out = vr->filters_out.out;

	vr->stat_cache_entries = g_ptr_array_sized_new(2);

	vr->job_queue_link.data = vr;

	action_stack_init(&vr->action_stack);

	return vr;
}

void vrequest_free(vrequest* vr) {
	guint i;

	action_stack_clear(vr, &vr->action_stack);
	plugins_handle_vrclose(vr);
	g_ptr_array_free(vr->plugin_ctx, TRUE);

	request_clear(&vr->request);
	physical_clear(&vr->physical);
	response_clear(&vr->response);
	environment_clear(&vr->env);

	filters_clean(vr, &vr->filters_in);
	filters_clean(vr, &vr->filters_out);

	if (g_atomic_int_get(&vr->queued)) { /* atomic access shouldn't be needed here; no one else can access vr here... */
		g_queue_unlink(&vr->wrk->job_queue, &vr->job_queue_link);
		g_atomic_int_set(&vr->queued, 0);
	}

	g_slice_free1(vr->wrk->srv->option_def_values->len * sizeof(option_value), vr->options);


	for (i = 0; i < vr->stat_cache_entries->len; i++) {
		stat_cache_entry *sce = g_ptr_array_index(vr->stat_cache_entries, i);
		stat_cache_entry_release(vr, sce);
	}
	g_ptr_array_free(vr->stat_cache_entries, TRUE);

	vr->ref->vr = NULL;
	if (g_atomic_int_dec_and_test(&vr->ref->refcount)) {
		g_slice_free(vrequest_ref, vr->ref);
	}

	g_slice_free(vrequest, vr);
}

void vrequest_reset(vrequest *vr) {
	guint i;

	action_stack_reset(vr, &vr->action_stack);
	plugins_handle_vrclose(vr);
	{
		gint len = vr->plugin_ctx->len;
		g_ptr_array_set_size(vr->plugin_ctx, 0);
		g_ptr_array_set_size(vr->plugin_ctx, len);
	}

	vr->state = VRS_CLEAN;

	vr->backend = NULL;

	request_reset(&vr->request);
	physical_reset(&vr->physical);
	response_reset(&vr->response);
	environment_reset(&vr->env);

	filters_reset(vr, &vr->filters_in);
	filters_reset(vr, &vr->filters_out);

	if (g_atomic_int_get(&vr->queued)) { /* atomic access shouldn't be needed here; no one else can access vr here... */
		g_queue_unlink(&vr->wrk->job_queue, &vr->job_queue_link);
		g_atomic_int_set(&vr->queued, 0);
	}

	for (i = 0; i < vr->stat_cache_entries->len; i++) {
		stat_cache_entry *sce = g_ptr_array_index(vr->stat_cache_entries, i);
		stat_cache_entry_release(vr, sce);
	}

	memcpy(vr->options, vr->wrk->srv->option_def_values->data, vr->wrk->srv->option_def_values->len * sizeof(option_value));

	if (1 != g_atomic_int_get(&vr->ref->refcount)) {
		/* If we are not the only user of vr->ref we have to get a new one and detach the old */
		vr->ref->vr = NULL;
		if (g_atomic_int_dec_and_test(&vr->ref->refcount)) {
			g_slice_free(vrequest_ref, vr->ref);
		}
		vr->ref = g_slice_new0(vrequest_ref);
		vr->ref->refcount = 1;
		vr->ref->vr = vr;
	}
}

vrequest_ref* vrequest_acquire_ref(vrequest *vr) {
	vrequest_ref* vr_ref = vr->ref;
	g_assert(vr_ref->refcount > 0);
	g_atomic_int_inc(&vr_ref->refcount);
	return vr_ref;
}

vrequest* vrequest_release_ref(vrequest_ref *vr_ref) {
	vrequest *vr = vr_ref->vr;
	g_assert(vr_ref->refcount > 0);
	if (g_atomic_int_dec_and_test(&vr_ref->refcount)) {
		g_assert(vr == NULL); /* we are the last user, and the ref holded by vr itself is handled extra, so the vr was already reset */
		g_slice_free(vrequest_ref, vr_ref);
	}
	return vr;
}

void vrequest_error(vrequest *vr) {
	vr->state = VRS_ERROR;
	vr->out->is_closed = TRUE;
	vrequest_joblist_append(vr);
}

void vrequest_backend_error(vrequest *vr, backend_error berror) {
	vr->action_stack.backend_failed = TRUE;
	vr->action_stack.backend_error = berror;
	vr->state = VRS_HANDLE_REQUEST_HEADERS;
	vr->backend = NULL;
	vrequest_joblist_append(vr);
}

void vrequest_backend_overloaded(vrequest *vr) {
	vrequest_backend_error(vr, BACKEND_OVERLOAD);
}
void vrequest_backend_dead(vrequest *vr) {
	vrequest_backend_error(vr, BACKEND_DEAD);
}


/* received all request headers */
void vrequest_handle_request_headers(vrequest *vr) {
	if (VRS_CLEAN == vr->state) {
		vr->state = VRS_HANDLE_REQUEST_HEADERS;
	}
	vrequest_joblist_append(vr);
}

/* received (partial) request content */
void vrequest_handle_request_body(vrequest *vr) {
	if (VRS_READ_CONTENT <= vr->state) {
		vrequest_joblist_append(vr);
	}
}

/* received all response headers/status code - call once from your indirect handler */
void vrequest_handle_response_headers(vrequest *vr) {
	if (VRS_HANDLE_RESPONSE_HEADERS > vr->state) {
		vr->state = VRS_HANDLE_RESPONSE_HEADERS;
	}
	vrequest_joblist_append(vr);
}

/* received (partial) response content - call from your indirect handler */
void vrequest_handle_response_body(vrequest *vr) {
	if (VRS_WRITE_CONTENT == vr->state) {
		vrequest_joblist_append(vr);
	}
}

/* response completely ready */
gboolean vrequest_handle_direct(vrequest *vr) {
	if (vr->state < VRS_READ_CONTENT) {
		vr->state = VRS_HANDLE_RESPONSE_HEADERS;
		vr->out->is_closed = TRUE;
		vr->backend = NULL;
		return TRUE;
	} else {
		return FALSE;
	}
}

/* handle request over time */
gboolean vrequest_handle_indirect(vrequest *vr, plugin *p) {
	if (vr->state < VRS_READ_CONTENT) {
		vr->state = VRS_READ_CONTENT;
		vr->backend = p;
		return TRUE;
	} else {
		return FALSE;
	}
}

gboolean vrequest_is_handled(vrequest *vr) {
	return vr->state >= VRS_READ_CONTENT;
}

static gboolean vrequest_do_handle_actions(vrequest *vr) {
	handler_t res = action_execute(vr);
	switch (res) {
	case HANDLER_GO_ON:
		if (vr->state == VRS_HANDLE_REQUEST_HEADERS) {
			/* request not handled */
			vrequest_handle_direct(vr);
			vr->response.http_status = 404;
			if (CORE_OPTION(CORE_OPTION_DEBUG_REQUEST_HANDLING).boolean) {
				VR_DEBUG(vr, "%s", "actions didn't handle request");
			}
			return TRUE;
		}
		/* otherwise state already changed */
		break;
	case HANDLER_COMEBACK:
		vrequest_joblist_append(vr); /* come back later */
		return FALSE;
	case HANDLER_WAIT_FOR_EVENT:
		return FALSE;
	case HANDLER_ERROR:
		vrequest_error(vr);
		return FALSE;
	}
	return TRUE;
}


static gboolean vrequest_do_handle_read(vrequest *vr) {
	if (vr->backend && vr->backend->handle_request_body) {
		if (!filters_run(vr, &vr->filters_in)) {
			vrequest_error(vr);
		}

		if (vr->vr_in->is_closed) vr->in->is_closed = TRUE;
		switch (vr->backend->handle_request_body(vr, vr->backend)) {
		case HANDLER_GO_ON:
			break;
		case HANDLER_COMEBACK:
			vrequest_joblist_append(vr); /* come back later */
			return FALSE;
		case HANDLER_WAIT_FOR_EVENT:
			return FALSE;
		case HANDLER_ERROR:
			vrequest_error(vr);
			break;
		}
	} else {
		chunkqueue_skip_all(vr->vr_in);
		if (vr->vr_in->is_closed) vr->in->is_closed = TRUE;
	}
	return TRUE;
}

static gboolean vrequest_do_handle_write(vrequest *vr) {
	if (!filters_run(vr, &vr->filters_out)) {
		vrequest_error(vr);
	}

	switch (vr->handle_response_body(vr)) {
	case HANDLER_GO_ON:
		break;
	case HANDLER_COMEBACK:
		vrequest_joblist_append(vr); /* come back later */
		return FALSE;
	case HANDLER_WAIT_FOR_EVENT:
		return FALSE;
	case HANDLER_ERROR:
		vrequest_error(vr);
		break;
	}
	return TRUE;
}

void vrequest_state_machine(vrequest *vr) {
	gboolean done = FALSE;
	handler_t res;
	do {
		switch (vr->state) {
		case VRS_CLEAN:
			done = TRUE;
			break;

		case VRS_HANDLE_REQUEST_HEADERS:
			if (CORE_OPTION(CORE_OPTION_DEBUG_REQUEST_HANDLING).boolean) {
				VR_DEBUG(vr, "%s", "handle request header");
			}
			if (!vrequest_do_handle_actions(vr)) return;
			res = vr->handle_request_headers(vr);
			switch (res) {
			case HANDLER_GO_ON:
				if (vr->state == VRS_HANDLE_REQUEST_HEADERS) {
					/* unhandled request */
					vr->response.http_status = 404;
					vrequest_handle_direct(vr);
				}
				break;
			case HANDLER_COMEBACK:
				vrequest_joblist_append(vr); /* come back later */
				done = TRUE;
				break;
			case HANDLER_WAIT_FOR_EVENT:
				done = (vr->state == VRS_HANDLE_REQUEST_HEADERS);
				break;
			case HANDLER_ERROR:
				vrequest_error(vr);
				break;
			}
			break;

		case VRS_READ_CONTENT:
			done = !vrequest_do_handle_read(vr);
			done = done || (vr->state == VRS_READ_CONTENT);
			break;

		case VRS_HANDLE_RESPONSE_HEADERS:
			if (!vrequest_do_handle_actions(vr)) return;
			res = vr->handle_response_headers(vr);
			switch (res) {
			case HANDLER_GO_ON:
				vr->state = VRS_WRITE_CONTENT;
				break;
			case HANDLER_COMEBACK:
				vrequest_joblist_append(vr); /* come back later */
				done = TRUE;
				break;
			case HANDLER_WAIT_FOR_EVENT:
				done = (vr->state == VRS_HANDLE_REQUEST_HEADERS);
				break;
			case HANDLER_ERROR:
				vrequest_error(vr);
				break;
			}
			break;

		case VRS_WRITE_CONTENT:
			vrequest_do_handle_read(vr);
			vrequest_do_handle_write(vr);
			done = TRUE;
			break;

		case VRS_ERROR:
			/* this will probably reset the vrequest, so stop handling after it */
			vr->handle_response_error(vr);
			return;
		}
	} while (!done);
}

void vrequest_joblist_append(vrequest *vr) {
	worker *wrk = vr->wrk;
	GQueue *const q = &wrk->job_queue;
	if (!g_atomic_int_compare_and_exchange(&vr->queued, 0, 1)) return; /* already in queue */
	g_queue_push_tail_link(q, &vr->job_queue_link);
	ev_timer_start(wrk->loop, &wrk->job_queue_watcher);
}

void vrequest_joblist_append_async(vrequest *vr) {
	worker *wrk = vr->wrk;
	GAsyncQueue *const q = wrk->job_async_queue;
	if (!g_atomic_int_compare_and_exchange(&vr->queued, 0, 1)) return; /* already in queue */
	g_async_queue_push(q, vrequest_acquire_ref(vr));
	ev_async_send(wrk->loop, &wrk->job_async_queue_watcher);
}

gboolean vrequest_stat(vrequest *vr) {
	/* Do not stat again */
	if (vr->physical.have_stat || vr->physical.have_errno) return vr->physical.have_stat;

	if (-1 == stat(vr->physical.path->str, &vr->physical.stat)) {
		vr->physical.have_stat = FALSE;
		vr->physical.have_errno = TRUE;
		vr->physical.stat_errno = errno;
		switch (errno) {
		case EACCES:
		case ENOENT:
		case ENOTDIR:
			break;
		default:
			VR_DEBUG(vr, "stat(%s) failed: %s (%d)", vr->physical.path->str, g_strerror(vr->physical.stat_errno), vr->physical.stat_errno);
		}
		return FALSE;
	}

	vr->physical.have_stat = TRUE;
	return TRUE;
}

gboolean vrequest_redirect(vrequest *vr, GString *uri) {
	if (!vrequest_handle_direct(vr))
		return FALSE;

	vr->response.http_status = 301;
	http_header_overwrite(vr->response.headers, CONST_STR_LEN("Location"), GSTR_LEN(uri));

	return TRUE;
}

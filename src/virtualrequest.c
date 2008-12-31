
#include <lighttpd/base.h>
#include <lighttpd/plugin_core.h>

static filter* filter_new() {
	filter *f = g_slice_new0(filter);
	return f;
}

static void filter_free(filter *f) {
	g_slice_free(filter, f);
}

static void filters_init(filters *fs) {
	fs->queue = g_ptr_array_new();
	fs->in = chunkqueue_new();
	fs->out = chunkqueue_new();
}

static void filters_clean(filters *fs) {
	guint i;
	for (i = 0; i < fs->queue->len; i++) {
		filter_free((filter*) g_ptr_array_index(fs->queue, i));
	}
	g_ptr_array_free(fs->queue, TRUE);
	chunkqueue_free(fs->in);
	chunkqueue_free(fs->out);
}

static void filters_reset(filters *fs) {
	guint i;
	for (i = 0; i < fs->queue->len; i++) {
		filter_free((filter*) g_ptr_array_index(fs->queue, i));
	}
	g_ptr_array_set_size(fs->queue, 0);
	chunkqueue_reset(fs->in);
	chunkqueue_reset(fs->out);
}

vrequest* vrequest_new(connection *con, vrequest_handler handle_response_headers, vrequest_handler handle_response_body, vrequest_handler handle_response_error, vrequest_handler handle_request_headers) {
	server *srv = con->srv;
	vrequest *vr = g_slice_new0(vrequest);

	vr->con = con;
	vr->state = VRS_CLEAN;

	vr->handle_response_headers = handle_response_headers;
	vr->handle_response_body = handle_response_body;
	vr->handle_response_error = handle_response_error;
	vr->handle_request_headers = handle_request_headers;

	vr->options = g_slice_copy(srv->option_def_values->len * sizeof(option_value), srv->option_def_values->data);

	request_init(&vr->request);
	physical_init(&vr->physical);
	response_init(&vr->response);

	filters_init(&vr->filters_in);
	filters_init(&vr->filters_out);
	vr->vr_in = vr->filters_in.in;
	vr->in = vr->filters_in.out;
	vr->out = vr->filters_out.in;
	vr->vr_out = vr->filters_out.out;

	action_stack_init(&vr->action_stack);

	return vr;
}

void vrequest_free(vrequest* vr) {
	request_clear(&vr->request);
	physical_clear(&vr->physical);
	response_clear(&vr->response);

	filters_clean(&vr->filters_in);
	filters_clean(&vr->filters_out);

	action_stack_clear(vr, &vr->action_stack);

	if (vr->job_queue_link) {
		g_queue_delete_link(&vr->con->wrk->job_queue, vr->job_queue_link);
		vr->job_queue_link = NULL;
	}

	g_slice_free1(vr->con->srv->option_def_values->len * sizeof(option_value), vr->options);

	g_slice_free(vrequest, vr);
}

void vrequest_reset(vrequest *vr) {
	vr->state = VRS_CLEAN;

	vr->handle_request_body = NULL;

	request_reset(&vr->request);
	physical_reset(&vr->physical);
	response_reset(&vr->response);

	filters_reset(&vr->filters_in);
	filters_reset(&vr->filters_out);

	action_stack_reset(vr, &vr->action_stack);

	if (vr->job_queue_link) {
		g_queue_delete_link(&vr->con->wrk->job_queue, vr->job_queue_link);
		vr->job_queue_link = NULL;
	}

	memcpy(vr->options, vr->con->srv->option_def_values->data, vr->con->srv->option_def_values->len * sizeof(option_value));
}

void vrequest_error(vrequest *vr) {
	vr->state = VRS_ERROR;
	vrequest_joblist_append(vr);
}

void vrequest_backend_overloaded(vrequest *vr) {
	vr->action_stack.backend_failed = TRUE;
	vr->action_stack.backend_error = BACKEND_OVERLOAD;
}

void vrequest_backend_error(vrequest *vr, backend_error berror) {
	vr->action_stack.backend_failed = TRUE;
	vr->action_stack.backend_error = berror;
}

void vrequest_backend_dead(vrequest *vr) {
	vr->action_stack.backend_failed = TRUE;
	vr->action_stack.backend_error = BACKEND_DEAD;
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
		vr->handle_request_body = NULL;
		return TRUE;
	} else {
		return FALSE;
	}
}

/* handle request over time */
gboolean vrequest_handle_indirect(vrequest *vr, vrequest_handler handle_request_body) {
	if (vr->state < VRS_READ_CONTENT) {
		vr->state = VRS_READ_CONTENT;
		vr->handle_request_body = handle_request_body;
		return TRUE;
	} else {
		return FALSE;
	}
}

static gboolean vrequest_do_handle_actions(vrequest *vr) {
	handler_t res = action_execute(vr);
	switch (res) {
	case HANDLER_GO_ON:
		if (vr->state == VRS_HANDLE_REQUEST_HEADERS) {
			VR_ERROR(vr, "%s", "actions didn't handle request");
			/* request not handled */
			vrequest_error(vr);
			return FALSE;
		}
		/* otherwise state already changed */
		break;
	case HANDLER_COMEBACK:
		vrequest_joblist_append(vr); /* come back later */
		return FALSE;
	case HANDLER_WAIT_FOR_FD: /* TODO: wait for fd */
	case HANDLER_WAIT_FOR_EVENT:
		return FALSE;
	case HANDLER_ERROR:
		vrequest_error(vr);
		return FALSE;
	}
	return TRUE;
}

static gboolean vrequest_do_handle_read(vrequest *vr) {
	handler_t res;
	if (vr->in->is_closed && vr->in->bytes_in == vr->in->bytes_out) return TRUE;
	if (vr->handle_request_body) {
		chunkqueue_steal_all(vr->in, vr->vr_in); /* TODO: filters */
		if (vr->vr_in->is_closed) vr->in->is_closed = TRUE;
		res = vr->handle_request_body(vr);
		switch (res) {
		case HANDLER_GO_ON:
			break;
		case HANDLER_COMEBACK:
			vrequest_joblist_append(vr); /* come back later */
			return FALSE;
		case HANDLER_WAIT_FOR_FD: /* TODO: wait for fd */
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
	handler_t res;
	chunkqueue_steal_all(vr->vr_out, vr->out); /* TODO: filters */
	if (vr->out->is_closed) vr->vr_out->is_closed = TRUE;
	res = vr->handle_response_body(vr);
	switch (res) {
	case HANDLER_GO_ON:
		break;
	case HANDLER_COMEBACK:
		vrequest_joblist_append(vr); /* come back later */
		return FALSE;
	case HANDLER_WAIT_FOR_FD: /* TODO: wait for fd */
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
			case HANDLER_WAIT_FOR_FD: /* TODO: wait for fd */
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
			case HANDLER_WAIT_FOR_FD: /* TODO: wait for fd */
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
			vr->handle_response_error(vr);
			return;
		}
	} while (!done);
}

void vrequest_joblist_append(vrequest *vr) {
	GQueue *const q = &vr->con->wrk->job_queue;
	worker *wrk = vr->con->wrk;
	if (vr->job_queue_link) return; /* already in queue */
	g_queue_push_tail(q, vr);
	vr->job_queue_link = g_queue_peek_tail_link(q);
	ev_timer_start(wrk->loop, &wrk->job_queue_watcher);
}


#include <lighttpd/base.h>
#include <lighttpd/filter.h>

static void li_filter_stop(liFilter *filter);

void li_vrequest_filters_init(liVRequest *vr) {
	vr->filters_in_last = vr->filters_out_last = NULL;
	vr->filters_in_first = vr->filters_out_first = NULL;
	vr->filters = g_ptr_array_new();
}

void li_vrequest_filters_clear(liVRequest *vr) {
	li_vrequest_filters_reset(vr);
	g_ptr_array_free(vr->filters, TRUE);
	vr->filters = NULL;
}

void li_vrequest_filters_reset(liVRequest *vr) {
	while (vr->filters->len > 0) {
		li_filter_stop(g_ptr_array_index(vr->filters, vr->filters->len - 1));
	}
	vr->filters_in_last = vr->filters_out_last = NULL;
	vr->filters_in_first = vr->filters_out_first = NULL;
}

static void filter_handle_data(liFilter *filter) {
	if (NULL != filter->handle_data) {
		goffset curoutlen = filter->stream.out->length;
		gboolean curout_closed = filter->stream.out->is_closed;

		LI_FORCE_ASSERT(NULL != filter->out);

		switch (filter->handle_data(filter->vr, filter)) {
		case LI_HANDLER_GO_ON:
			if (NULL != filter->stream.source && (0 != filter->stream.source->out->length)) {
				/* auto comeback */
				li_stream_again(&filter->stream);
			}
			break;
		case LI_HANDLER_COMEBACK:
			li_stream_again(&filter->stream);
			break;
		case LI_HANDLER_WAIT_FOR_EVENT:
			break;
		case LI_HANDLER_ERROR:
			filter->in = NULL;
			if (NULL != filter->vr) li_vrequest_error(filter->vr);
			li_stream_reset(&filter->stream);
			break;
		}

		if (NULL != filter->stream.source && (0 == filter->stream.source->out->length) && filter->stream.source->out->is_closed) {
			li_stream_disconnect(&filter->stream);
		}

		if (curoutlen != filter->stream.out->length || curout_closed != filter->stream.out->is_closed) {
			li_stream_notify(&filter->stream);
		}
	}
}

static void filter_stream_cb(liStream *stream, liStreamEvent event) {
	liFilter *filter = LI_CONTAINER_OF(stream, liFilter, stream);

	switch (event) {
	case LI_STREAM_NEW_DATA:
		if (NULL != filter->handle_event) {
			filter->handle_event(filter->vr, filter, event);
		}
		filter_handle_data(filter);
		break;
	case LI_STREAM_NEW_CQLIMIT:
		if (NULL != filter->handle_event) {
			filter->handle_event(filter->vr, filter, event);
		}
		break;
	case LI_STREAM_CONNECTED_SOURCE:
		filter->in = (NULL != filter->stream.source) ? filter->stream.source->out : NULL;
		if (NULL != filter->handle_event) {
			filter->handle_event(filter->vr, filter, event);
		} else {
			li_stream_again(stream);
		}
		break;
	case LI_STREAM_CONNECTED_DEST:
		if (NULL != filter->handle_event) {
			filter->handle_event(filter->vr, filter, event);
		}
		break;
	case LI_STREAM_DISCONNECTED_SOURCE:
		filter->in = NULL;
		if (NULL != filter->handle_event) {
			filter->handle_event(filter->vr, filter, event);
		} else {
			if (!stream->out->is_closed) li_stream_again(stream);
		}
		break;
	case LI_STREAM_DISCONNECTED_DEST:
		if (NULL != filter->handle_event) {
			filter->handle_event(filter->vr, filter, event);
		} else {
			stream->out->is_closed = TRUE;
			li_chunkqueue_skip_all(stream->out);
		}
		break;
	case LI_STREAM_DESTROY:
		filter->out = NULL;
		if (NULL != filter->handle_event) {
			filter->handle_event(filter->vr, filter, event);
		}
		if (NULL != filter->handle_free) {
			filter->handle_free(filter->vr, filter);
		}
		g_slice_free(liFilter, filter);
		break;
	}
}

liFilter* li_filter_new(liVRequest* vr, liFilterHandlerCB handle_data, liFilterFreeCB handle_free, liFilterEventCB handle_event, gpointer param) {
	liFilter *f;

	f = g_slice_new0(liFilter);
	li_stream_init(&f->stream, &vr->wrk->loop, filter_stream_cb);
	f->out = f->stream.out;

	f->param = param;
	f->handle_data = handle_data;
	f->handle_free = handle_free;
	f->handle_event = handle_event;

	f->vr = vr;
	f->filter_ndx = vr->filters->len;
	g_ptr_array_add(vr->filters, f);

	return f;
}

static void li_filter_stop(liFilter *filter) {
	liVRequest *vr = filter->vr;

	if (NULL == vr) return;

	filter->vr = NULL;

	/* remove from vr filters list */
	LI_FORCE_ASSERT(vr->filters->len > 0);
	if (vr->filters->len - 1 != filter->filter_ndx) {
		/* not the last filter, swap: */
		liFilter *last = g_ptr_array_index(vr->filters, vr->filters->len - 1);
		last->filter_ndx = filter->filter_ndx;
		g_ptr_array_index(vr->filters, filter->filter_ndx) = last;
	}
	g_ptr_array_set_size(vr->filters, vr->filters->len - 1);

	li_stream_again(&filter->stream);

	li_stream_release(&filter->stream);
}


liFilter* li_vrequest_add_filter_in(liVRequest *vr, liFilterHandlerCB handle_data, liFilterFreeCB handle_free, liFilterEventCB handle_event, gpointer param) {
	liFilter *f;

	/* as soon as we have a backend -> too late for in filters */
	if (LI_VRS_READ_CONTENT <= vr->state) return NULL;

	f = li_filter_new(vr, handle_data, handle_free, handle_event, param);

	if (NULL == vr->filters_in_first) {
		LI_FORCE_ASSERT(NULL == vr->filters_in_last);

		vr->filters_in_first = &f->stream;
		vr->filters_in_last = &f->stream;
	} else {
		LI_FORCE_ASSERT(NULL != vr->filters_in_last);

		li_stream_connect(vr->filters_in_last, &f->stream);
		vr->filters_in_last = &f->stream;
	}

	return f;
}

liFilter* li_vrequest_add_filter_out(liVRequest *vr, liFilterHandlerCB handle_data, liFilterFreeCB handle_free, liFilterEventCB handle_event, gpointer param) {
	liFilter *f;

	/* as soon as we write the response it is too late for out filters */
	if (LI_VRS_WRITE_CONTENT <= vr->state) return NULL;

	f = li_filter_new(vr, handle_data, handle_free, handle_event, param);

	if (NULL == vr->filters_out_first) {
		LI_FORCE_ASSERT(NULL == vr->filters_out_last);

		vr->filters_out_first = &f->stream;
		vr->filters_out_last = &f->stream;
	} else {
		LI_FORCE_ASSERT(NULL != vr->filters_out_last);

		li_stream_connect(vr->filters_out_last, &f->stream);
		vr->filters_out_last = &f->stream;
	}

	return f;
}

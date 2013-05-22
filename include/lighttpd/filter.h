#ifndef _LIGHTTPD_FILTER_H_
#define _LIGHTTPD_FILTER_H_

#ifndef _LIGHTTPD_BASE_H_
#error Please include <lighttpd/base.h> instead of this file
#endif

typedef liHandlerResult (*liFilterHandlerCB)(liVRequest *vr, liFilter *f);
typedef void (*liFilterFreeCB)(liVRequest *vr, liFilter *f);
typedef void (*liFilterEventCB)(liVRequest *vr, liFilter *f, liStreamEvent event);

struct liFilter {
	liStream stream;

	liChunkQueue *in, *out;

	/* if the handler wasn't able to handle all "in" data it must call li_stream_again(&f->stream) to trigger a new call to handle_data
	 * vr, in and out can be NULL if the associated vrequest/stream was destroyed
	 */
	liFilterHandlerCB handle_data;
	liFilterFreeCB handle_free;
	liFilterEventCB handle_event;
	gpointer param;

	liVRequest *vr;
	guint filter_ndx;
};

LI_API liFilter* li_filter_new(liVRequest *vr, liFilterHandlerCB handle_data, liFilterFreeCB handle_free, liFilterEventCB handle_event, gpointer param);

LI_API liFilter* li_vrequest_add_filter_in(liVRequest *vr, liFilterHandlerCB handle_data, liFilterFreeCB handle_free, liFilterEventCB handle_event, gpointer param);
LI_API liFilter* li_vrequest_add_filter_out(liVRequest *vr, liFilterHandlerCB handle_data, liFilterFreeCB handle_free, liFilterEventCB handle_event, gpointer param);

LI_API void li_vrequest_filters_init(liVRequest *vr);
LI_API void li_vrequest_filters_clear(liVRequest *vr);
LI_API void li_vrequest_filters_reset(liVRequest *vr);

#endif

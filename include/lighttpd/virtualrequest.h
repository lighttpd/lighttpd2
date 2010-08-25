#ifndef _LIGHTTPD_VIRTUALREQUEST_H_
#define _LIGHTTPD_VIRTUALREQUEST_H_

#ifndef _LIGHTTPD_BASE_H_
#error Please include <lighttpd/base.h> instead of this file
#endif

typedef enum {
	/* waiting for request headers */
	LI_VRS_CLEAN,

	/* all headers received, now handling them, set up input filters
	 *   this state is set by the previous vrequest after VRS_WROTE_RESPONSE_HEADERS (or the main connection),
	 *   and the handle_request function is called (which execute the action stack by default)
	 */
	LI_VRS_HANDLE_REQUEST_HEADERS,

	/* request headers handled, input filters ready; now content is accepted
	 *   this state is set via handle_indirect (handle_direct skips to LI_VRS_HANDLE_RESPONSE_HEADERS
	 */
	LI_VRS_READ_CONTENT,

	/* all response headers written, now set up output filters */
	LI_VRS_HANDLE_RESPONSE_HEADERS,

	/* output filters ready, content can be written */
	LI_VRS_WRITE_CONTENT,

	/* request done */
/* 	VRS_END, */

	LI_VRS_ERROR
} liVRequestState;

typedef liHandlerResult (*liFilterHandlerCB)(liVRequest *vr, liFilter *f);
typedef void (*liFilterFreeCB)(liVRequest *vr, liFilter *f);
typedef liHandlerResult (*liVRequestHandlerCB)(liVRequest *vr);
typedef liHandlerResult (*liVRequestPluginHandlerCB)(liVRequest *vr, liPlugin *p);
typedef gboolean (*liVRequestCheckIOCB)(liVRequest *vr);

struct liConCallbacks {
	liVRequestHandlerCB
		handle_request_headers,
		handle_response_headers, handle_response_body,
		handle_response_error; /* this is _not_ for 500 - internal error */

	liVRequestCheckIOCB handle_check_io;
};

/* this data "belongs" to a vrequest, but is updated by the connection code */
struct liConInfo {
	const liConCallbacks *callbacks;

	liSocketAddress remote_addr, local_addr;
	GString *remote_addr_str, *local_addr_str;
	gboolean is_ssl;
	gboolean keep_alive;

	/* bytes in our "raw-io-out-queue" that hasn't be sent yet. (whatever "sent" means - in ssl buffer, kernel, ...) */
	goffset out_queue_length;

	/* use li_vrequest_update_stats_{in,out} to update this */
	struct {
		guint64 bytes_in; /* total number of bytes received */
		guint64 bytes_out; /* total number of bytes sent */
		ev_tstamp last_avg;
		guint64 bytes_in_5s; /* total number of bytes received at last 5s interval */
		guint64 bytes_out_5s; /* total number of bytes sent at last 5s interval */
		guint64 bytes_in_5s_diff; /* diff between bytes received at 5s interval n and interval n-1 */
		guint64 bytes_out_5s_diff; /* diff between bytes sent at 5s interval n and interval n-1 */
	} stats;
};

struct liFilter {
	liChunkQueue *in, *out;
	liFilterHandlerCB handle_data;
	liFilterFreeCB handle_free;
	gpointer param;
	/* do not modify these yourself: */
	gboolean knows_out_is_closed, done;
};

struct liFilters {
	GPtrArray *queue;
	liChunkQueue *in, *out;
};

struct liVRequestRef {
	gint refcount;
	liWorker *wrk;
	liVRequest *vr; /* This is only accesible by the worker thread the vrequest belongs to, and it may be NULL if the vrequest is already reset */
};

struct liVRequest {
	liConInfo *coninfo;
	liWorker *wrk;
	liVRequestRef *ref;

	liOptionValue *options;
	liOptionPtrValue **optionptrs;

	liVRequestState state;

	ev_tstamp ts_started;

	GPtrArray *plugin_ctx;
	liPlugin *backend;

	liRequest request;
	liPhysical physical;
	liResponse response;

	/* environment entries will be passed to the backends */
	liEnvironment env;

	/* -> vr_in -> filters_in -> in_memory ->(buffer_on_disk) -> in -> handle -> out -> filters_out -> vr_out -> */
	gboolean cq_memory_limit_hit; /* stop feeding chunkqueues with memory chunks */
	liFilters filters_in, filters_out;
	liChunkQueue *vr_in, *vr_out, *in_memory;
	liChunkQueue *in, *out;
	liFilterBufferOnDiskState in_buffer_state;

	liActionStack action_stack;
	gboolean actions_wait_for_response;

	gint queued;
	GList job_queue_link;

	GPtrArray *stat_cache_entries;

	/* I/O throttling */
	gboolean throttled; /* TRUE if vrequest is throttled */
	struct {
		gint magazine; /* currently available for use */

		struct {
			liThrottlePool *ptr; /* NULL if not in any throttling pool */
			GList lnk;
			GQueue *queue;
			gint magazine;
		} pool;
		struct {
			gchar unused; /* this struct is unused for now */
		} ip;
		struct {
			guint rate; /* maximum transfer rate in bytes per second, 0 if unlimited */
			ev_tstamp last_update;
		} con;
		liWaitQueueElem wqueue_elem;
	} throttle;
};

#define VREQUEST_WAIT_FOR_RESPONSE_HEADERS(vr) \
	do { \
		if (vr->state == LI_VRS_HANDLE_REQUEST_HEADERS) { \
			VR_ERROR(vr, "%s", "Cannot wait for response headers as no backend handler found - fix your config"); \
			return LI_HANDLER_ERROR; \
		} else if (vr->state < LI_VRS_HANDLE_RESPONSE_HEADERS) { \
			return LI_HANDLER_WAIT_FOR_EVENT; \
		} \
	} while (0)

LI_API liVRequest* li_vrequest_new(liWorker *wrk, liConInfo *coninfo);
LI_API void li_vrequest_free(liVRequest *vr);
/* if keepalive = TRUE, you either have to reset it later again with FALSE or call li_vrequest_start before reusing the vr;
 * keepalive = TRUE doesn't reset the vr->request fields, so mod_status can show the last request data in the keep-alive state
 */
LI_API void li_vrequest_reset(liVRequest *vr, gboolean keepalive);

LI_API liVRequestRef* li_vrequest_get_ref(liVRequest *vr);
LI_API void li_vrequest_ref_acquire(liVRequestRef *vr_ref);
LI_API liVRequest* li_vrequest_ref_release(liVRequestRef *vr_ref);

LI_API liFilter* li_vrequest_add_filter_in(liVRequest *vr, liFilterHandlerCB handle_data, liFilterFreeCB handle_free, gpointer param);
LI_API liFilter* li_vrequest_add_filter_out(liVRequest *vr, liFilterHandlerCB handle_data, liFilterFreeCB handle_free, gpointer param);

/* Signals an internal error; handles the error in the _next_ loop */
LI_API void li_vrequest_error(liVRequest *vr);

LI_API void li_vrequest_backend_overloaded(liVRequest *vr);
LI_API void li_vrequest_backend_dead(liVRequest *vr);
LI_API void li_vrequest_backend_error(liVRequest *vr, liBackendError berror);
LI_API void li_vrequest_backend_finished(liVRequest *vr); /* action.c */

/* resets fields which weren't reset in favor of keep-alive tracking */
LI_API void li_vrequest_start(liVRequest *vr);
/* received all request headers */
LI_API void li_vrequest_handle_request_headers(liVRequest *vr);
/* received (partial) request content */
LI_API void li_vrequest_handle_request_body(liVRequest *vr);
/* received all response headers/status code - call once from your indirect handler */
LI_API void li_vrequest_handle_response_headers(liVRequest *vr);
/* received (partial) response content - call from your indirect handler */
LI_API void li_vrequest_handle_response_body(liVRequest *vr);

/* response completely ready */
LI_API gboolean li_vrequest_handle_direct(liVRequest *vr);
/* handle request over time */
LI_API gboolean li_vrequest_handle_indirect(liVRequest *vr, liPlugin *p);
LI_API gboolean li_vrequest_is_handled(liVRequest *vr);

LI_API void li_vrequest_state_machine(liVRequest *vr);
LI_API void li_vrequest_joblist_append(liVRequest *vr);
LI_API void li_vrequest_joblist_append_async(liVRequestRef *vr_ref);

LI_API gboolean li_vrequest_redirect(liVRequest *vr, GString *uri);

LI_API gboolean li_vrequest_redirect_directory(liVRequest *vr);

/* updates worker stats too */
LI_API void li_vrequest_update_stats_in(liVRequest *vr, goffset transferred);
LI_API void li_vrequest_update_stats_out(liVRequest *vr, goffset transferred);

#endif

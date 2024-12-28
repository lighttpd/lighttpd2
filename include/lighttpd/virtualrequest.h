#ifndef _LIGHTTPD_VIRTUALREQUEST_H_
#define _LIGHTTPD_VIRTUALREQUEST_H_

#ifndef _LIGHTTPD_BASE_H_
#error Please include <lighttpd/base.h> instead of this file
#endif

#include <lighttpd/jobqueue.h>

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

typedef void (*liVRequestHandlerCB)(liVRequest *vr);
typedef liThrottleState* (*liVRequestThrottleCB)(liVRequest *vr);
typedef void (*liVRequestConnectionUpgradeCB)(liVRequest *vr, liStream *backend_drain, liStream *backend_source);

struct liConCallbacks {
	liVRequestHandlerCB handle_response_error; /* this is _not_ for 500 - internal error */
	liVRequestThrottleCB throttle_out, throttle_in;
	liVRequestConnectionUpgradeCB connection_upgrade;
};

/* this data "belongs" to a vrequest, but is updated by the connection code */
struct liConInfo {
	const liConCallbacks *callbacks;

	liSocketAddress remote_addr, local_addr;
	GString *remote_addr_str, *local_addr_str;
	gboolean is_ssl;
	gboolean keep_alive;
	gboolean aborted; /* network aborted connection before response was sent completely */

	liStream *req;
	liStream *resp;

	/* bytes in our "raw-io-out-queue" that hasn't be sent yet. (whatever "sent" means - in ssl buffer, kernel, ...) */
	goffset out_queue_length;

	/* use li_vrequest_update_stats_{in,out} to update this */
	struct {
		guint64 bytes_in; /* total number of bytes received */
		guint64 bytes_out; /* total number of bytes sent */
		li_tstamp last_avg;
		guint64 bytes_in_5s; /* total number of bytes received at last 5s interval */
		guint64 bytes_out_5s; /* total number of bytes sent at last 5s interval */
		guint64 bytes_in_5s_diff; /* diff between bytes received at 5s interval n and interval n-1 */
		guint64 bytes_out_5s_diff; /* diff between bytes sent at 5s interval n and interval n-1 */
	} stats;
};

struct liVRequest {
	liConInfo *coninfo;
	liWorker *wrk;

	liOptionValue *options;
	liOptionPtrValue **optionptrs;

	liLogContext log_context;

	liVRequestState state;

	li_tstamp ts_started;

	GPtrArray *plugin_ctx;

	liRequest request;
	liPhysical physical;
	liResponse response;

	/* environment entries will be passed to the backends */
	liEnvironment env;
	/* request specific table; `REQ` global in lua */
	int lua_server_env_ref; /* for srv->LL */
	int lua_worker_env_ref; /* for wrk->LL */

	/* -> vr_in -> filters_in -> in_memory ->(buffer_on_disk) -> in -> handle -> out -> filters_out -> vr_out -> */
	GPtrArray *filters;
	liStream *filters_in_last, *filters_out_last;
	liStream *filters_in_first, *filters_out_first;

	liStream *in_buffer_on_disk_stream, *wait_for_request_body_stream;

	liPlugin *backend;
	liStream *backend_source;
	liStream *backend_drain;
	liChunkQueue *direct_out; /* NULL for indirect responses, backend_source->out for direct responses. do not set this yourself for indirect responses! */

	liActionStack action_stack;

	liJob job;

	GPtrArray *stat_cache_entries;
};

#define LI_VREQUEST_WAIT_FOR_REQUEST_BODY(vr) \
	do { \
		if (!li_vrequest_wait_for_request_body(vr)) { \
			return LI_HANDLER_WAIT_FOR_EVENT; \
		} \
	} while (0)

#define LI_VREQUEST_WAIT_FOR_RESPONSE_HEADERS(vr) \
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

/****************************************************/
/* called by connection                             */
/****************************************************/
/* resets fields which weren't reset in favor of keep-alive tracking */
LI_API void li_vrequest_start(liVRequest *vr);
/* received all request headers */
LI_API void li_vrequest_handle_request_headers(liVRequest *vr);

/* called by connection IO handling */
LI_API void li_vrequest_update_stats_in(liVRequest *vr, goffset transferred);
LI_API void li_vrequest_update_stats_out(liVRequest *vr, goffset transferred);

/****************************************************/
/* called by actions handling the request           */
/****************************************************/
/* returns TRUE if request body is present
 * or shouldn't be waited for (if caching on disk is disabled and liCQLimit hit, ...)
 * if it returns FALSE it will trigger li_vrequest_joblist_append later */
LI_API gboolean li_vrequest_wait_for_request_body(liVRequest *vr);

/* response completely ready; use this only in action callbacks */
LI_API gboolean li_vrequest_handle_direct(liVRequest *vr);
/* check whether the request is already handled */
LI_API gboolean li_vrequest_is_handled(liVRequest *vr);

/* handle request over time */
LI_API gboolean li_vrequest_handle_indirect(liVRequest *vr, liPlugin *p);
/* signal that backend connection is ready - after this a backend error might result in a internal error */
LI_API void li_vrequest_indirect_connect(liVRequest *vr, liStream *backend_drain, liStream *backend_source);
/* received all response headers/status code - call once from your indirect handler */
LI_API void li_vrequest_indirect_headers_ready(liVRequest *vr);

/* call instead of headers_ready */
LI_API void li_vrequest_connection_upgrade(liVRequest *vr, liStream *backend_drain, liStream *backend_source);

/* Signals an internal error; handles the error in the _next_ loop */
LI_API void li_vrequest_error(liVRequest *vr);

LI_API void li_vrequest_backend_overloaded(liVRequest *vr);
LI_API void li_vrequest_backend_dead(liVRequest *vr);
LI_API void li_vrequest_backend_error(liVRequest *vr, liBackendError berror);
LI_API void li_vrequest_backend_finished(liVRequest *vr); /* action.c */



LI_API void li_vrequest_state_machine(liVRequest *vr);
LI_API void li_vrequest_joblist_append(liVRequest *vr);
LI_API liJobRef* li_vrequest_get_ref(liVRequest *vr);

LI_API gboolean li_vrequest_redirect(liVRequest *vr, GString *uri);
LI_API gboolean li_vrequest_redirect_directory(liVRequest *vr);


#endif

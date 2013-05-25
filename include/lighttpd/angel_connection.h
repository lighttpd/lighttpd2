#ifndef _LIGHTTPD_ANGEL_CONNECTION_H_
#define _LIGHTTPD_ANGEL_CONNECTION_H_

#include <lighttpd/idlist.h>
#include <lighttpd/events.h>

#define ANGEL_CALL_MAX_STR_LEN (64*1024) /* must fit into a gint32 */

typedef struct liAngelConnection liAngelConnection;

typedef struct liAngelCall liAngelCall;

/* error, data and fds-array will be freed/closed by the angel api itself; if you want to use the fds set the array size to 0 */
typedef void (*liAngelCallCB)(gpointer ctx, gboolean timeout, GString *error, GString *data, GArray *fds);

typedef void (*liAngelReceiveCallCB)(liAngelConnection *acon,
	const gchar *mod, gsize mod_len, const gchar *action, gsize action_len,
	gint32 id,
	GString *data);

/* gets called after read/write errors */
typedef void (*liAngelCloseCB)(liAngelConnection *acon, GError *err);

struct liAngelConnection {
	gpointer data;
	GMutex *mutex;
	int fd;
	liIDList *call_id_list;
	GPtrArray *call_table;
	liEventIO fd_watcher;
	liEventAsync out_notify_watcher;
	GQueue *out;
	liAngelBuffer in;

	liAngelReceiveCallCB recv_call;
	liAngelCloseCB close_cb;

	/* parse input */
	struct {
		gboolean have_header;
		gint32 type, id;
		gint32 mod_len, action_len, error_len, data_len, missing_fds;
		guint body_size;
		GString *mod, *action, *error, *data;
		GArray *fds;
	} parse;
};

/* with multi-threading you should protect the structure
 * containing the angel_call with a lock
 */
struct liAngelCall {
	gpointer context;
	liAngelCallCB callback;
	/* internal data */
	gint32 id; /* id is -1 if there is no call pending (the callback may still be running) */
	liAngelConnection *acon;
	liEventTimer timeout_watcher;
	liEventAsync result_watcher;

	struct {
		GString *error, *data;
		GArray *fds;
	} result;
};

/* error handling */
#define LI_ANGEL_CALL_ERROR li_angel_call_error_quark()
LI_API GQuark li_angel_call_error_quark(void);

#define LI_ANGEL_CONNECTION_ERROR li_angel_connection_error_quark()
LI_API GQuark li_angel_connection_error_quark(void);

typedef enum {
	LI_ANGEL_CALL_ALREADY_RUNNING,              /* the angel_call struct is already in use for a call */
	LI_ANGEL_CALL_OUT_OF_CALL_IDS,              /* too many calls already pending */
	LI_ANGEL_CALL_INVALID                       /* invalid params */
} liAngelCallError;

typedef enum {
	LI_ANGEL_CONNECTION_CLOSED,                 /* error on socket */
	LI_ANGEL_CONNECTION_RESET,                  /* connection closed by remote side */
	LI_ANGEL_CONNECTION_INVALID_DATA            /* invalid data from stream */
} liAngelConnectionError;

/* create connection */
LI_API liAngelConnection* li_angel_connection_new(
	liEventLoop *loop, int fd, gpointer data,
	liAngelReceiveCallCB recv_call, liAngelCloseCB close_cb);
LI_API void li_angel_connection_free(liAngelConnection *acon);


LI_API liAngelCall *li_angel_call_new(liEventLoop *loop, liAngelCallCB callback, li_tstamp timeout);
/* returns TRUE if a call was cancelled; make sure you don't call free while you're calling send_call */
LI_API gboolean li_angel_call_free(liAngelCall *call);

/* calls */
/* the GString* parameters get stolen by the angel call (moved to chunkqueue) */
LI_API gboolean li_angel_send_simple_call(
	liAngelConnection *acon,
	const gchar *mod, gsize mod_len, const gchar *action, gsize action_len,
	GString *data,
	GError **err);

LI_API gboolean li_angel_send_call(
	liAngelConnection *acon,
	const gchar *mod, gsize mod_len, const gchar *action, gsize action_len,
	liAngelCall *call,
	GString *data,
	GError **err);

LI_API gboolean li_angel_send_result(
	liAngelConnection *acon,
	gint32 id,
	GString *error, GString *data, GArray *fds,
	GError **err);

/* free temporary needed memroy; call this once in while after some activity */
LI_API void li_angel_cleanup_tables(liAngelConnection *acon);

/* Usage */
#if 0
void init(void) {
	/* ... init ctx... */
	angel_call_init(&ctx->call);
	ctx->call.context = ctx;
	ctx->call.callback = my_callback;
}

gboolean start_call(curctx) {
	GError *err = NULL;
	GString *data;
	lock();
	ctx = get_ctx();
	/* another thread may have already called angel, depending on the task (e.g. fastcgi spawn) */
	if (!angel_call_is_needed(ctx)) {
		unlock();
		return TRUE;
	}
	data = build_call_data();
	if (!li_angel_send_call(acon, CONST_STR_LEN("mymod"), CONST_STR_LEN("myaction"), &ctx->call, 10, data, &err)) {
		unlock();
		report_error(&err);
		return FALSE;
	}
	/* add current context (e.g. vrequest) to a wakeup list */
	push_to_queue(ctx->waitqueue, curctx);
	
	unlock();
	return TRUE; /* at this point the callback may be already finished */
}

void my_callback(angel_call *acall, gboolean timeout, GString *error, GString *data, GArray *fds) {
	lock();
	
	handle_error();
	parse_data();
	/* ... */
	
done:
	wakeup(acall->ctx->waitqueue);
	if (error) g_string_free(error, TRUE);
	if (data) g_string_free(error, TRUE);
	/* destroy fd-array? */
	
	perhaps_free_ctx(acall->ctx);
	
	unlock();
}

void stop_call(void) {
	lock();
	ctx = get_ctx();
	if (!angel_cancel_call(acon, ctx)) {
		/* callback either is already done or just to be called */
		/* do _not_ destroy the context */
		unlock();
		return;
	}
	
	perhaps_free_ctx(ctx);
	unlock();
}
#endif

#endif

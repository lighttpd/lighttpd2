#ifndef _LIGHTTPD_ANGEL_CONNECTION_H_
#define _LIGHTTPD_ANGEL_CONNECTION_H_

#include <lighttpd/idlist.h>

#define ANGEL_CALL_MAX_STR_LEN (64*1024) /* must fit into a gint32 */

struct angel_connection;
typedef struct angel_connection angel_connection;

struct angel_call;
typedef struct angel_call angel_call;

typedef void (*AngelCallback)(gpointer ctx, gboolean timeout, GString *error, GString *data, GArray *fds);

typedef void (*AngelReceiveCall)(angel_connection *acon,
	const gchar *mod, gsize mod_len, const gchar *action, gsize action_len,
	gint32 id,
	GString *data);

typedef void (*AngelReceiveResult)(angel_connection *acon,
	const gchar *mod, gsize mod_len, const gchar *action, gsize action_len,
	gint32 id,
	GString *error, GString *data, GArray *fds);

/* gets called after read/write errors */
typedef void (*AngelCloseCallback)(angel_connection *acon, GError *err);

struct angel_connection {
	gpointer data;
	GStaticMutex mutex; /* angel itself has no threads */
	struct ev_loop *loop;
	int fd;
	idlist *call_id_list;
	GPtrArray *call_table;
	ev_io fd_watcher;
	ev_async out_notify_watcher;
	GQueue *out;
	angel_buffer in;

	AngelReceiveCall recv_call;
	AngelReceiveResult recv_result;
	AngelCloseCallback close_cb;

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
struct angel_call {
	gpointer context;
	AngelCallback callback;
	/* internal data */
	gint32 id; /* id is -1 if there is no call pending (the callback may still be running) */
	angel_connection *acon;
	ev_timer timeout_watcher;
};

/* error handling */
#define ANGEL_CALL_ERROR angel_call_error_quark()
LI_API GQuark angel_call_error_quark();

#define ANGEL_CONNECTION_ERROR angel_connection_error_quark()
LI_API GQuark angel_connection_error_quark();

typedef enum {
	ANGEL_CALL_ALREADY_RUNNING,              /* the angel_call struct is already in use for a call */
	ANGEL_CALL_OUT_OF_CALL_IDS,              /* too many calls already pending */
	ANGEL_CALL_INVALID                       /* invalid params */
} AngelCallError;

typedef enum {
	ANGEL_CONNECTION_CLOSED,                 /* error on socket */
	ANGEL_CONNECTION_INVALID_DATA            /* invalid data from stream */
} AngelConnectionError;

/* create connection */
LI_API angel_connection* angel_connection_create(
	struct ev_loop *loop, int fd, gpointer data,
	AngelReceiveCall recv_call, AngelReceiveResult recv_result, AngelCloseCallback close_cb);

LI_API angel_call *angel_call_create(AngelCallback callback, ev_tstamp timeout);
/* returns TRUE if a call was cancelled; make sure you don't call free while you're calling send_call */
LI_API gboolean angel_call_free(angel_call *call);

/* calls */
/* the GString* parameters get stolen by the angel call (moved to chunkqueue) */
LI_API gboolean angel_send_simple_call(
	angel_connection *acon,
	const gchar *mod, gsize mod_len, const gchar *action, gsize action_len,
	GString *data,
	GError **err);

LI_API gboolean angel_send_call(
	angel_connection *acon,
	const gchar *mod, gsize mod_len, const gchar *action, gsize action_len,
	angel_call *call,
	GString *data,
	GError **err);

LI_API gboolean angel_send_result(
	angel_connection *acon,
	const gchar *mod, gsize mod_len, const gchar *action, gsize action_len,
	gint32 id,
	GString *error, GString *data, GArray *fds,
	GError **err);

/* free temporary needed memroy; call this once in while after some activity */
LI_API void angel_cleanup_tables(angel_connection *acon);

/* Usage */
#if 0
void init() {
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
	if (!angel_send_call(acon, CONST_STR_LEN("mymod"), CONST_STR_LEN("myaction"), &ctx->call, 10, data, &err)) {
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

void stop_call() {
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

#ifndef _LIGHTTPD_ANGEL_CONNECTION_H_
#define _LIGHTTPD_ANGEL_CONNECTION_H_

struct angel_connection;
typedef struct angel_connection angel_connection;

struct angel_call;
typedef struct angel_call angel_call;

typedef void (*AngelCallback)(angel_call *acall, gboolean timeout, GString *error, GString *data, GArray *fds);


struct angel_connection {
	GStaticMutex mutex; /* angel itself has no threads */
	struct ev_loop *loop;
	int fd;
};

/* with multi-threading you should protect the structure
 * containing the angel_call with a lock
 */
struct angel_call {
	gpointer context;
	AngelCallback callback;
	/* internal data */
	gint32 id; /* id is -1 if there is no call pending (the callback may still be running) */
	guint timeout;
	ev_timer timeout_watcher;
	ev_io fd_watcher;
};

/* error handling */
#define ANGEL_CALL_ERROR angel_call_error_quark()
LI_API GQuark angel_call_error_quark();

typedef enum {
	ANGEL_CALL_ALREADY_RUNNING               /* the angel_call struct is already in use for a call */
} AngelCallError;

/* create connection */
LI_API angel_connection* angel_connection_create(int fd);


/* calls */
/* the GString* parameters get stolen by the angel call (moved to chunkqueue) */
LI_API void angel_call_init(angel_call *call);

LI_API gboolean angel_send_simple_call(
	angel_connection *acon,
	const gchar *mod, gsize mod_len, const gchar *action, gsize action_len,
	GString *data,
	GError **err);

LI_API gboolean angel_send_call(
	angel_connection *acon,
	const gchar *mod, gsize mod_len, const gchar *action, gsize action_len,
	angel_call *call, guint timeout,
	GString *data,
	GError **err);

LI_API gboolean angel_send_result(
	angel_connection *acon,
	const gchar *mod, gsize mod_len, const gchar *action, gsize action_len,
	angel_call *call, guint timeout,
	GString *error, GString *data, GArray *fds,
	GError **err);

LI_API gboolean angel_cancel_call(angel_connection *acon, angel_call *call);

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

/*
 * mod_fastcgi - connect to fastcgi backends for generating content
 *
 * Description:
 *     mod_fastcgi connects to a backend over tcp or unix sockets
 *
 * Setups:
 *     none
 * Options:
 *     fastcgi.log_plain_errors <value> - whether to prepend timestamp and other info to
 *                                        fastcgi stderr lines in the "backend" log.
 *         type: boolean
 * Actions:
 *     fastcgi <socket>  - connect to backend at <socket>
 *         socket: string, either "ip:port" or "unix:/path"
 *
 * Example config:
 *     fastcgi "127.0.0.1:9090"
 *
 * Todo:
 *     - reuse fastcgi connections (keepalive)
 *     - option for alternative doc-root?
 *
 * Author:
 *     Copyright (c) 2009 Stefan Bühler
 */

#include <lighttpd/base.h>
#include <lighttpd/plugin_core.h>

#include <lighttpd/backends.h>
#include "fastcgi_stream.h"

enum fastcgi_options_t {
	FASTCGI_OPTION_LOG_PLAIN_ERRORS = 0,
};

LI_API gboolean mod_fastcgi_init(liModules *mods, liModule *mod);
LI_API gboolean mod_fastcgi_free(liModules *mods, liModule *mod);


typedef struct fastcgi_context fastcgi_context;

struct fastcgi_context {
	gint refcount;
	liPlugin *plugin;

	liFastCGIBackendPool *pool;

	GString *socket_str;
};

static void fastcgi_context_release(fastcgi_context *ctx) {
	if (!ctx) return;
	assert(g_atomic_int_get(&ctx->refcount) > 0);
	if (g_atomic_int_dec_and_test(&ctx->refcount)) {
		li_fastcgi_backend_pool_free(ctx->pool);
		g_string_free(ctx->socket_str, TRUE);
		g_slice_free(fastcgi_context, ctx);
	}
}

static void fastcgi_context_acquire(fastcgi_context *ctx) {
	assert(g_atomic_int_get(&ctx->refcount) > 0);
	g_atomic_int_inc(&ctx->refcount);
}


static void fastcgi_con_reset_cb(liVRequest *vr, liFastCGIBackendPool *pool, liFastCGIBackendConnection *bcon) {
	fastcgi_context *ctx = (fastcgi_context*) bcon->data;
	UNUSED(pool);

	li_fastcgi_backend_put(bcon);
	li_vrequest_error(vr);
	fastcgi_context_release(ctx);
}
static void fastcgi_con_end_request_cb(liVRequest *vr, liFastCGIBackendPool *pool, liFastCGIBackendConnection *bcon, guint32 appStatus) {
	fastcgi_context *ctx = (fastcgi_context*) bcon->data;
	UNUSED(vr);
	UNUSED(pool);
	UNUSED(appStatus);

	li_fastcgi_backend_put(bcon);
	fastcgi_context_release(ctx);
}
static void fastcgi_con_stderr_cb(liVRequest *vr, liFastCGIBackendPool *pool, liFastCGIBackendConnection *bcon, GString *message) {
	fastcgi_context *ctx = (fastcgi_context*) bcon->data;
	liPlugin *p = ctx->plugin;
	UNUSED(pool);

	if (OPTION(FASTCGI_OPTION_LOG_PLAIN_ERRORS).boolean) {
		li_log_split_lines(vr->wrk->srv, vr->wrk, &vr->log_context, LI_LOG_LEVEL_BACKEND, 0, message->str, "");
	} else {
		VR_BACKEND_LINES(vr, message->str, "(fcgi-stderr %s) ", ctx->socket_str->str);
	}
}

static const liFastCGIBackendCallbacks fcgi_callbacks = {
	fastcgi_con_reset_cb,
	fastcgi_con_end_request_cb,
	fastcgi_con_stderr_cb
};


static liHandlerResult fastcgi_handle_abort(liVRequest *vr, gpointer param, gpointer context) {
	fastcgi_context *ctx = (fastcgi_context*) param;
	liFastCGIBackendWait *bwait = context;

	if (bwait != NULL) {
		li_fastcgi_backend_wait_stop(vr, ctx->pool, &bwait);
	}

	return LI_HANDLER_GO_ON;
}

static liHandlerResult fastcgi_handle(liVRequest *vr, gpointer param, gpointer *context) {
	fastcgi_context *ctx = (fastcgi_context*) param;
	liFastCGIBackendWait *bwait = *context;
	liFastCGIBackendConnection *bcon;

	if (li_vrequest_is_handled(vr)) return LI_HANDLER_GO_ON;

	LI_VREQUEST_WAIT_FOR_REQUEST_BODY(vr);

	switch (li_fastcgi_backend_get(vr, ctx->pool, &bcon, &bwait)) {
	case LI_BACKEND_SUCCESS:
		assert(NULL == bwait);
		assert(NULL != bcon);
		*context = bwait;
		break;
	case LI_BACKEND_WAIT:
		assert(NULL != bwait);
		*context = bwait;

		return LI_HANDLER_WAIT_FOR_EVENT;
	case LI_BACKEND_TIMEOUT:
		li_vrequest_backend_dead(vr);
		return LI_HANDLER_GO_ON;
	}

	fastcgi_context_acquire(ctx);

	bcon->data = ctx;

	return LI_HANDLER_GO_ON;
}

static void fastcgi_free(liServer *srv, gpointer param) {
	fastcgi_context *ctx = (fastcgi_context*) param;
	UNUSED(srv);

	ERROR(srv, "%s", "fastcgi_free");
	fastcgi_context_release(ctx);
}

static liAction* fastcgi_create(liServer *srv, liWorker *wrk, liPlugin* p, liValue *val, gpointer userdata) {
	liFastCGIBackendConfig config;
	fastcgi_context *ctx;

	UNUSED(wrk); UNUSED(userdata);

	if (val->type != LI_VALUE_STRING) {
		ERROR(srv, "%s", "fastcgi expects a string as parameter");
		return FALSE;
	}

	config.sock_addr = li_sockaddr_from_string(val->data.string, 0);
	if (NULL == config.sock_addr.addr) {
		ERROR(srv, "Invalid socket address '%s'", val->data.string->str);
		return NULL;
	}

	ctx = g_slice_new0(fastcgi_context);
	ctx->refcount = 1;

	config.callbacks = &fcgi_callbacks;
	config.max_connections = 0;
	config.max_requests = 0;
	config.connect_timeout = 5;
	config.wait_timeout = 5;
	config.idle_timeout = 5;
	config.disable_time = 0;

	ctx->pool = li_fastcgi_backend_pool_new(&config);
	li_sockaddr_clear(&config.sock_addr);

	ctx->plugin = p;
	ctx->socket_str = g_string_new_len(GSTR_LEN(val->data.string));

	return li_action_new_function(fastcgi_handle, fastcgi_handle_abort, fastcgi_free, ctx);
}

static const liPluginOption options[] = {
	{ "fastcgi.log_plain_errors", LI_VALUE_BOOLEAN, FALSE, NULL },

	{ NULL, 0, 0, NULL }
};

static const liPluginAction actions[] = {
	{ "fastcgi", fastcgi_create, NULL },
	{ NULL, NULL, NULL }
};

static const liPluginSetup setups[] = {
	{ NULL, NULL, NULL }
};


static void plugin_init(liServer *srv, liPlugin *p, gpointer userdata) {
	UNUSED(srv); UNUSED(userdata);

	p->options = options;
	p->actions = actions;
	p->setups = setups;
}


gboolean mod_fastcgi_init(liModules *mods, liModule *mod) {
	MODULE_VERSION_CHECK(mods);

	mod->config = li_plugin_register(mods->main, "mod_fastcgi", plugin_init, NULL);

	return mod->config != NULL;
}

gboolean mod_fastcgi_free(liModules *mods, liModule *mod) {
	if (mod->config)
		li_plugin_free(mods->main, mod->config);

	return TRUE;
}

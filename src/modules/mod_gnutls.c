
#include <lighttpd/base.h>
#include <lighttpd/throttle.h>

#include "gnutls_filter.h"

#include <gnutls/gnutls.h>
#include <glib-2.0/glib/galloca.h>


#if GNUTLS_VERSION_NUMBER >= 0x020a00
#define HAVE_SESSION_TICKET
#endif

LI_API gboolean mod_gnutls_init(liModules *mods, liModule *mod);
LI_API gboolean mod_gnutls_free(liModules *mods, liModule *mod);


typedef struct mod_connection_ctx mod_connection_ctx;
typedef struct mod_context mod_context;

struct mod_connection_ctx {
	gnutls_session_t session;
	liConnection *con;
	mod_context *ctx;

	liGnuTLSFilter *tls_filter;

	liIOStream *sock_stream;
	gpointer simple_socket_data;
};

struct mod_context {
	gint refcount;

	gnutls_certificate_credentials_t server_cert;
	gnutls_priority_t server_priority;
	gnutls_priority_t server_priority_beast;
#ifdef HAVE_SESSION_TICKET
	gnutls_datum_t ticket_key;
#endif

	unsigned int protect_against_beast:1;
};

static void mod_gnutls_context_release(mod_context *ctx) {
	if (!ctx) return;
	assert(g_atomic_int_get(&ctx->refcount) > 0);
	if (g_atomic_int_dec_and_test(&ctx->refcount)) {
		gnutls_priority_deinit(ctx->server_priority_beast);
		gnutls_priority_deinit(ctx->server_priority);
		gnutls_certificate_free_credentials(ctx->server_cert);
#ifdef HAVE_SESSION_TICKET
		/* wtf. why is there no function in gnutls for this... */
		if (NULL != ctx->ticket_key.data) {
			gnutls_free(ctx->ticket_key.data);
			ctx->ticket_key.data = NULL;
			ctx->ticket_key.size = 0;
		}
#endif

		g_slice_free(mod_context, ctx);
	}
}

static void mod_gnutls_context_acquire(mod_context *ctx) {
	assert(g_atomic_int_get(&ctx->refcount) > 0);
	g_atomic_int_inc(&ctx->refcount);
}


static mod_context *mod_gnutls_context_new(liServer *srv) {
	mod_context *ctx = g_slice_new0(mod_context);
	int r;

	if (GNUTLS_E_SUCCESS != (r = gnutls_certificate_allocate_credentials(&ctx->server_cert))) {
		ERROR(srv, "gnutls_certificate_allocate_credentials failed(%s): %s",
			gnutls_strerror_name(r), gnutls_strerror(r));
		goto error0;
	}

	if (GNUTLS_E_SUCCESS != (r = gnutls_priority_init(&ctx->server_priority, "NORMAL", NULL))) {
		ERROR(srv, "gnutls_priority_init failed(%s): %s",
			gnutls_strerror_name(r), gnutls_strerror(r));
		goto error1;
	}

	if (GNUTLS_E_SUCCESS != (r = gnutls_priority_init(&ctx->server_priority_beast, "NORMAL:-CIPHER-ALL:+ARCFOUR-128", NULL))) {
		ERROR(srv, "gnutls_priority_init failed(%s): %s",
			gnutls_strerror_name(r), gnutls_strerror(r));
		goto error2;
	}

#ifdef HAVE_SESSION_TICKET
	if (GNUTLS_E_SUCCESS != (r = gnutls_session_ticket_key_generate(&ctx->ticket_key))) {
		ERROR(srv, "gnutls_session_ticket_key_generate failed(%s): %s",
			gnutls_strerror_name(r), gnutls_strerror(r));
		goto error3;
	}
#endif

	ctx->refcount = 1;
	ctx->protect_against_beast = 1;

	return ctx;

error3:
	gnutls_priority_deinit(ctx->server_priority_beast);

error2:
	gnutls_priority_deinit(ctx->server_priority);

error1:
	gnutls_certificate_free_credentials(ctx->server_cert);

error0:
	g_slice_free(mod_context, ctx);
	return NULL;
}

static void tcp_io_cb(liIOStream *stream, liIOStreamEvent event) {
	mod_connection_ctx *conctx = stream->data;
	assert(NULL == conctx->sock_stream || conctx->sock_stream == stream);

	if (LI_IOSTREAM_DESTROY == event) {
		li_stream_simple_socket_close(stream, TRUE); /* kill it, ssl sent an close alert message */
	}

	li_connection_simple_tcp(&conctx->con, stream, &conctx->simple_socket_data, event);

	if (NULL != conctx->con && conctx->con->out_has_all_data
	    && (NULL == stream->stream_out.out || 0 == stream->stream_out.out->length)
	    && li_streams_empty(conctx->con->con_sock.raw_out, NULL)) {
		li_stream_simple_socket_flush(stream);
		li_connection_request_done(conctx->con);
	}

	switch (event) {
	case LI_IOSTREAM_DESTROY:
		assert(NULL == conctx->sock_stream);
		assert(NULL == conctx->tls_filter);
		assert(NULL == conctx->con);
		stream->data = NULL;
		g_slice_free(mod_connection_ctx, conctx);
		return;
	default:
		break;
	}
}

static void handshake_cb(liGnuTLSFilter *f, gpointer data, liStream *plain_source, liStream *plain_drain) {
	mod_connection_ctx *conctx = data;
	liConnection *con = conctx->con;
	UNUSED(f);

	if (NULL != con) {
		li_stream_connect(plain_source, con->con_sock.raw_in);
		li_stream_connect(con->con_sock.raw_out, plain_drain);
	} else {
		li_stream_reset(plain_source);
		li_stream_reset(plain_drain);
	}
}

static void close_cb(liGnuTLSFilter *f, gpointer data) {
	mod_connection_ctx *conctx = data;
	liConnection *con = conctx->con;
	assert(conctx->tls_filter == f);

	conctx->tls_filter = NULL;
	li_gnutls_filter_free(f);
	gnutls_deinit(conctx->session);

	if (NULL != conctx->ctx) {
		mod_gnutls_context_release(conctx->ctx);
		conctx->ctx = NULL;
	}

	if (NULL != conctx->con) {
		liStream *raw_out = con->con_sock.raw_out, *raw_in = con->con_sock.raw_in;
		assert(con->con_sock.data == conctx);
		conctx->con = NULL;
		con->con_sock.data = NULL;
		li_stream_acquire(raw_in);
		li_stream_reset(raw_out);
		li_stream_reset(raw_in);
		li_stream_release(raw_in);
	}

	if (NULL != conctx->sock_stream) {
		liIOStream *stream = conctx->sock_stream;
		conctx->sock_stream = NULL;
		li_iostream_release(stream);
	}
}

static int post_client_hello_cb(liGnuTLSFilter *f, gpointer data) {
	mod_connection_ctx *conctx = data;
	gnutls_protocol_t p = gnutls_protocol_get_version(conctx->session);
	UNUSED(f);

	if (conctx->ctx->protect_against_beast) {
		if (GNUTLS_SSL3 == p || GNUTLS_TLS1_0 == p) {
			gnutls_priority_set(conctx->session, conctx->ctx->server_priority_beast);
		}
	}

	return GNUTLS_E_SUCCESS;
}

static const liGnuTLSFilterCallbacks filter_callbacks = {
	handshake_cb,
	close_cb,
	post_client_hello_cb
};

static void gnutlc_tcp_finished(liConnection *con, gboolean aborted) {
	mod_connection_ctx *conctx = con->con_sock.data;
	UNUSED(aborted);

	con->info.is_ssl = FALSE;
	con->con_sock.callbacks = NULL;

	if (NULL != conctx) {
		assert(con == conctx->con);
		close_cb(conctx->tls_filter, conctx);
		assert(NULL == con->con_sock.data);
	}

	{
		liStream *raw_out = con->con_sock.raw_out, *raw_in = con->con_sock.raw_in;
		con->con_sock.raw_out = con->con_sock.raw_in = NULL;
		if (NULL != raw_out) { li_stream_reset(raw_out); li_stream_release(raw_out); }
		if (NULL != raw_in) { li_stream_reset(raw_in); li_stream_release(raw_in); }
	}
}

static liThrottleState* gnutls_tcp_throttle_out(liConnection *con) {
	mod_connection_ctx *conctx = con->con_sock.data;
	if (NULL == conctx) return NULL;
	if (NULL == conctx->sock_stream->throttle_out) conctx->sock_stream->throttle_out = li_throttle_new();
	return conctx->sock_stream->throttle_out;
}

static liThrottleState* gnutls_tcp_throttle_in(liConnection *con) {
	mod_connection_ctx *conctx = con->con_sock.data;
	if (NULL == conctx) return NULL;
	if (NULL == conctx->sock_stream->throttle_in) conctx->sock_stream->throttle_in = li_throttle_new();
	return conctx->sock_stream->throttle_in;
}

static const liConnectionSocketCallbacks gnutls_tcp_cbs = {
	gnutlc_tcp_finished,
	gnutls_tcp_throttle_out,
	gnutls_tcp_throttle_in
};

static gboolean mod_gnutls_con_new(liConnection *con, int fd) {
	liEventLoop *loop = &con->wrk->loop;
	liServer *srv = con->srv;
	mod_context *ctx = con->srv_sock->data;
	mod_connection_ctx *conctx;
	gnutls_session_t session;
	int r;

	if (GNUTLS_E_SUCCESS != (r = gnutls_init(&session, GNUTLS_SERVER))) {
		ERROR(srv, "gnutls_init (%s): %s",
			gnutls_strerror_name(r), gnutls_strerror(r));
		return FALSE;
	}

	mod_gnutls_context_acquire(ctx);

	if (GNUTLS_E_SUCCESS != (r = gnutls_priority_set(session, ctx->server_priority))) {
		ERROR(srv, "gnutls_priority_set (%s): %s",
			gnutls_strerror_name(r), gnutls_strerror(r));
		goto fail;
	}
	if (GNUTLS_E_SUCCESS != (r = gnutls_credentials_set(session, GNUTLS_CRD_CERTIFICATE, ctx->server_cert))) {
		ERROR(srv, "gnutls_credentials_set (%s): %s",
			gnutls_strerror_name(r), gnutls_strerror(r));
		goto fail;
	}

#ifdef HAVE_SESSION_TICKET
	if (GNUTLS_E_SUCCESS != (r = gnutls_session_ticket_enable_server(session, &ctx->ticket_key))) {
		ERROR(srv, "gnutls_session_ticket_enable_server (%s): %s",
			gnutls_strerror_name(r), gnutls_strerror(r));
		goto fail;
	}
#endif

	conctx = g_slice_new0(mod_connection_ctx);
	conctx->session = session;
	conctx->sock_stream = li_iostream_new(con->wrk, fd, tcp_io_cb, conctx);
	conctx->tls_filter = li_gnutls_filter_new(srv, con->wrk, &filter_callbacks, conctx, conctx->session,
		&conctx->sock_stream->stream_in, &conctx->sock_stream->stream_out);
	conctx->con = con;
	conctx->ctx = ctx;

	con->con_sock.data = conctx;
	con->con_sock.callbacks = &gnutls_tcp_cbs;
	con->con_sock.raw_out = li_stream_plug_new(loop);
	con->con_sock.raw_in = li_stream_plug_new(loop);
	con->info.is_ssl = TRUE;

	return TRUE;

fail:
	gnutls_deinit(session);
	mod_gnutls_context_release(ctx);

	return FALSE;
}


static void mod_gnutls_sock_release(liServerSocket *srv_sock) {
	mod_context *ctx = srv_sock->data;

	if (!ctx) return;

	mod_gnutls_context_release(ctx);
}

static void gnutls_setup_listen_cb(liServer *srv, int fd, gpointer data) {
	mod_context *ctx = data;
	liServerSocket *srv_sock;
	UNUSED(data);

	if (-1 == fd) {
		mod_gnutls_context_release(ctx);
		return;
	}

	srv_sock = li_server_listen(srv, fd);

	srv_sock->data = ctx; /* transfer ownership, no refcount change */

	srv_sock->new_cb = mod_gnutls_con_new;
	srv_sock->release_cb = mod_gnutls_sock_release;
}

static gboolean gnutls_setup(liServer *srv, liPlugin* p, liValue *val, gpointer userdata) {
	mod_context *ctx;
	GHashTableIter hti;
	gpointer hkey, hvalue;
	GString *htkey;
	liValue *htval;
	int r;

	/* setup defaults */
	GString *ipstr = NULL;
	const char
		*priority = NULL,
		*pemfile = NULL, *ca_file = NULL;
	gboolean
		protect_against_beast = TRUE;

	UNUSED(p); UNUSED(userdata);

	if (val->type != LI_VALUE_HASH) {
		ERROR(srv, "%s", "gnutls expects a hash as parameter");
		return FALSE;
	}

	g_hash_table_iter_init(&hti, val->data.hash);
	while (g_hash_table_iter_next(&hti, &hkey, &hvalue)) {
		htkey = hkey; htval = hvalue;

		if (g_str_equal(htkey->str, "listen")) {
			if (htval->type != LI_VALUE_STRING) {
				ERROR(srv, "%s", "gnutls listen expects a string as parameter");
				return FALSE;
			}
			ipstr = htval->data.string;
		} else if (g_str_equal(htkey->str, "pemfile")) {
			if (htval->type != LI_VALUE_STRING) {
				ERROR(srv, "%s", "gnutls pemfile expects a string as parameter");
				return FALSE;
			}
			pemfile = htval->data.string->str;
		} else if (g_str_equal(htkey->str, "ca-file")) {
			if (htval->type != LI_VALUE_STRING) {
				ERROR(srv, "%s", "gnutls ca-file expects a string as parameter");
				return FALSE;
			}
			ca_file = htval->data.string->str;
		} else if (g_str_equal(htkey->str, "priority")) {
			if (htval->type != LI_VALUE_STRING) {
				ERROR(srv, "%s", "gnutls priority expects a string as parameter");
				return FALSE;
			}
			priority = htval->data.string->str;
		} else if (g_str_equal(htkey->str, "protect-against-beast")) {
			if (htval->type != LI_VALUE_BOOLEAN) {
				ERROR(srv, "%s", "gnutls protect-against-beast expects a boolean as parameter");
				return FALSE;
			}
			protect_against_beast = htval->data.boolean;
		}
	}

	if (!ipstr) {
		ERROR(srv, "%s", "gnutls needs a listen parameter");
		return FALSE;
	}

	if (!pemfile) {
		ERROR(srv, "%s", "gnutls needs a pemfile");
		return FALSE;
	}

	if (!(ctx = mod_gnutls_context_new(srv))) return FALSE;

	ctx->protect_against_beast = protect_against_beast;

	if (GNUTLS_E_SUCCESS != (r = gnutls_certificate_set_x509_key_file(ctx->server_cert, pemfile, pemfile, GNUTLS_X509_FMT_PEM))) {
		ERROR(srv, "gnutls_certificate_set_x509_key_file failed(certfile '%s', keyfile '%s', PEM) (%s): %s",
			pemfile, pemfile,
			gnutls_strerror_name(r), gnutls_strerror(r));
		goto error_free_ctx;
	}

	if ((NULL != ca_file) && 0 > (r = gnutls_certificate_set_x509_trust_file(ctx->server_cert, ca_file, GNUTLS_X509_FMT_PEM))) {
		ERROR(srv, "gnutls_certificate_set_x509_trust_file failed(cafile '%s', PEM) (%s): %s",
			ca_file,
			gnutls_strerror_name(r), gnutls_strerror(r));
		goto error_free_ctx;
	}

	if (priority) {
		const char *errpos = NULL;
		gnutls_priority_t prio;
		GString *s = srv->main_worker->tmp_str;

		if (GNUTLS_E_SUCCESS != (r = gnutls_priority_init(&prio, priority, &errpos))) {
			ERROR(srv, "gnutls_priority_init failed(priority '%s', error at '%s') (%s): %s",
				priority, errpos,
				gnutls_strerror_name(r), gnutls_strerror(r));
			goto error_free_ctx;
		}

		gnutls_priority_deinit(ctx->server_priority);
		ctx->server_priority = prio;

		if (protect_against_beast) {
			g_string_assign(s, priority);
			g_string_append_len(s, CONST_STR_LEN(":-CIPHER-ALL:+ARCFOUR-128"));
			if (GNUTLS_E_SUCCESS != (r = gnutls_priority_init(&prio, s->str, &errpos))) {
				ERROR(srv, "gnutls_priority_init failed(priority '%s', error at '%s') (%s): %s",
					s->str, errpos,
					gnutls_strerror_name(r), gnutls_strerror(r));
				goto error_free_ctx;
			}

			gnutls_priority_deinit(ctx->server_priority_beast);
			ctx->server_priority_beast = prio;
		}
	}

	li_angel_listen(srv, ipstr, gnutls_setup_listen_cb, ctx);

	return TRUE;

error_free_ctx:
	if (ctx) {
		mod_gnutls_context_release(ctx);
	}

	return FALSE;
}

static const liPluginOption options[] = {
	{ NULL, 0, 0, NULL }
};

static const liPluginAction actions[] = {
	{ NULL, NULL, NULL }
};

static const liPluginSetup setups[] = {
	{ "gnutls", gnutls_setup, NULL },

	{ NULL, NULL, NULL }
};


static void plugin_init(liServer *srv, liPlugin *p, gpointer userdata) {
	UNUSED(srv); UNUSED(userdata);

	p->options = options;
	p->actions = actions;
	p->setups = setups;
}

gboolean mod_gnutls_init(liModules *mods, liModule *mod) {
	MODULE_VERSION_CHECK(mods);

	gnutls_global_init();

	mod->config = li_plugin_register(mods->main, "mod_gnutls", plugin_init, NULL);

	return mod->config != NULL;
}

gboolean mod_gnutls_free(liModules *mods, liModule *mod) {
	if (mod->config)
		li_plugin_free(mods->main, mod->config);

	gnutls_global_deinit();

	return TRUE;
}

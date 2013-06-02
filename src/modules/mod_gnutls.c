
#include <lighttpd/base.h>
#include <lighttpd/throttle.h>

#include "gnutls_filter.h"
#include "ssl-session-db.h"

#include <gnutls/gnutls.h>
#include <glib-2.0/glib/galloca.h>


#if GNUTLS_VERSION_NUMBER >= 0x020a00
#define HAVE_SESSION_TICKET
#endif

LI_API gboolean mod_gnutls_init(liModules *mods, liModule *mod);
LI_API gboolean mod_gnutls_free(liModules *mods, liModule *mod);

static int load_dh_params_3247(gnutls_dh_params_t *dh_params);

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

	liSSLSessionDB *session_db;

	gnutls_certificate_credentials_t server_cert;
	gnutls_dh_params_t dh_params;
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
		gnutls_dh_params_deinit(ctx->dh_params);
#ifdef HAVE_SESSION_TICKET
		/* wtf. why is there no function in gnutls for this... */
		if (NULL != ctx->ticket_key.data) {
			gnutls_free(ctx->ticket_key.data);
			ctx->ticket_key.data = NULL;
			ctx->ticket_key.size = 0;
		}
#endif
		li_ssl_session_db_free(ctx->session_db);
		ctx->session_db = NULL;

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

static int session_db_store_cb(void *_sdb, gnutls_datum_t key, gnutls_datum_t data) {
	liSSLSessionDB *sdb = _sdb;
	li_ssl_session_db_store(sdb, key.data, key.size, data.data, data.size);
	return 0;
}
static int session_db_remove_cb(void *_sdb, gnutls_datum_t key) {
	liSSLSessionDB *sdb = _sdb;
	li_ssl_session_db_remove(sdb, key.data, key.size);
	return 0;
}
static gnutls_datum_t session_db_retrieve_cb(void *_sdb, gnutls_datum_t key) {
	liSSLSessionDB *sdb = _sdb;
	liSSLSessionDBData *data = li_ssl_session_db_lookup(sdb, key.data, key.size);
	gnutls_datum_t result = { NULL, 0 };
	if (NULL != data) {
		result.size = data->size;
		result.data = gnutls_malloc(result.size);
		memcpy(result.data, data->data, result.size);
		li_ssl_session_db_data_release(data);
	}
	return result;
}

static const liGnuTLSFilterCallbacks filter_callbacks = {
	handshake_cb,
	close_cb,
	post_client_hello_cb
};

static void gnutls_tcp_finished(liConnection *con, gboolean aborted) {
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
	gnutls_tcp_finished,
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

	if (NULL != ctx->session_db) {
		gnutls_db_set_ptr(session, ctx->session_db);
		gnutls_db_set_remove_function(session, session_db_remove_cb);
		gnutls_db_set_retrieve_function(session, session_db_retrieve_cb);
		gnutls_db_set_store_function(session, session_db_store_cb);
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
		*priority = NULL, *dh_params_file = NULL,
		*pemfile = NULL, *ca_file = NULL;
	gboolean
		protect_against_beast = TRUE;
	gint64 session_db_size = 256;

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
		} else if (g_str_equal(htkey->str, "dh-params")) {
			if (htval->type != LI_VALUE_STRING) {
				ERROR(srv, "%s", "gnutls dh-params expects a string as parameter");
				return FALSE;
			}
			dh_params_file = htval->data.string->str;
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
		} else if (g_str_equal(htkey->str, "session-db-size")) {
			if (htval->type != LI_VALUE_NUMBER) {
				ERROR(srv, "%s", "gnutls session-db-size expects an integer as parameter");
				return FALSE;
			}
			session_db_size = htval->data.number;
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

	if (session_db_size > 0) ctx->session_db = li_ssl_session_db_new(session_db_size);

	if (NULL != dh_params_file) {
		gchar *contents = NULL;
		gsize length = 0;
		GError *error = NULL;
		gnutls_datum_t pkcs3_params;

		if (GNUTLS_E_SUCCESS != (r = gnutls_dh_params_init(&ctx->dh_params))) {
			ERROR(srv, "gnutls_dh_params_init failed (%s): %s",
				gnutls_strerror_name(r), gnutls_strerror(r));
			goto error_free_ctx;
		}

		if (!g_file_get_contents(dh_params_file, &contents, &length, &error)) {
			ERROR(srv, "Unable to read dh-params file '%s': %s", dh_params_file, error->message);
			g_error_free(error);
			goto error_free_ctx;
		}

		pkcs3_params.data = (unsigned char*) contents;
		pkcs3_params.size = length;

		r = gnutls_dh_params_import_pkcs3(ctx->dh_params, &pkcs3_params, GNUTLS_X509_FMT_PEM);
		g_free(contents);

		if (GNUTLS_E_SUCCESS != r) {
			ERROR(srv, "couldn't load dh parameters from file '%s' (%s): %s",
				dh_params_file,
				gnutls_strerror_name(r), gnutls_strerror(r));
			goto error_free_ctx;
		}
	} else {
		if (GNUTLS_E_SUCCESS != (r = load_dh_params_3247(&ctx->dh_params))) {
			ERROR(srv, "couldn't load dh parameters(%s): %s",
				gnutls_strerror_name(r), gnutls_strerror(r));
			goto error_free_ctx;
		}

		gnutls_certificate_set_dh_params(ctx->server_cert, ctx->dh_params);
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

/*

-----BEGIN DH PARAMETERS-----
MIIDOAKCAZZFc1zRv5gu49kbADHiznc7EAFuz0jSlFfg3W/F290j1yvaACccKoXw
8zlujNkfVtiRExeyfTtKgQtBqQ9Dxb2+93RbkN77DEUc7fgmBXRt6H0qPuZ07whQ
wylUDaY+ZtHAFqitKmYel8u7Bb5wCdKCgDRutSD3ER6u1IzrL5QOs6UH0KMpB0Ap
h1eWu39HXdkDKIjeImEzs9GKqHSFcCv0m6kxxGH25Ez9KaCpevjz6q8TXPL7ASuo
qRLGzN4OHfcm/IYFF6Thfv22SJWLgDgF0nuOxyDQrUP1kXD1WA+Rr+1b7HYfZB6s
V+CaDAHD+N/u0V2nVeKf1jB+IsuFR/IrYpGuQxAy1od7oW3QSydy/X15cGD0GNPf
RLnUhLEqv3zanv6f/b2OnIiwIl3P3ffrlgKnOsCzktPY5kSy+JbuaEuJeE8clane
JFbweqS3BedGqvFRuQjNcnTZoAb8cP3S/8NHuABq7Ty6HpxDkuEUMlQhKcO36/+H
nbU9BDNWqbonty4Feoejx7muxlfbqrEBwg23nwwTAoIBljPgHZuDtXsmCmM0O05J
UsgevXhQbB1foaxw+dajOSP1eez/hLWPZaP1uH8hSj5ATpZ3NOwoLEOGelZ1TDLa
df3kUY1PO/wtxJXWuvUZmsR2iHkPs1AXvF3LeyooETZoKBtK3OR2UWnYga+6dYyo
uV+aAOewPOxAuLNJTIvXwJqVGvrGR5pGwdwNfHwFYL31/ygJn5+B7mI4aN9V26Ud
PVRUGqTxStA/bFX1On1zBRSsRdf0JCWpgt/UEReQ1iKKWjQ4zYm5zr9vN6T+h+T0
nZzkyjydHY2y6YWkb4ROISK239HmPTujzVZ0BQbyPYkA+R9ezgnriHACDBq1FUsC
FNcBVg/e2rOg32ktinogXEu85Ugzoj+HbKzg+1khetZiPN1r0KBb8X7WRaftzQ1v
A21L4ViU6M7crLNmHUDmPT5/pjnuIkgtZTQ7Sc50ueZX2Q+zpqsJFouVejW02eTv
kXh/iqDNxDJK0LaTyjfdfSyNlI4P+WXyTRomfWOYphp77llfRjKyt7Mnk7o6vRBW
n6HIaduaaW8CAgEA
-----END DH PARAMETERS-----

*/

static int load_dh_params_3247(gnutls_dh_params_t *dh_params) {
	static const unsigned char dh3247_p[]={
		0x45,0x73,0x5C,0xD1,0xBF,0x98,0x2E,0xE3,0xD9,0x1B,0x00,0x31,
		0xE2,0xCE,0x77,0x3B,0x10,0x01,0x6E,0xCF,0x48,0xD2,0x94,0x57,
		0xE0,0xDD,0x6F,0xC5,0xDB,0xDD,0x23,0xD7,0x2B,0xDA,0x00,0x27,
		0x1C,0x2A,0x85,0xF0,0xF3,0x39,0x6E,0x8C,0xD9,0x1F,0x56,0xD8,
		0x91,0x13,0x17,0xB2,0x7D,0x3B,0x4A,0x81,0x0B,0x41,0xA9,0x0F,
		0x43,0xC5,0xBD,0xBE,0xF7,0x74,0x5B,0x90,0xDE,0xFB,0x0C,0x45,
		0x1C,0xED,0xF8,0x26,0x05,0x74,0x6D,0xE8,0x7D,0x2A,0x3E,0xE6,
		0x74,0xEF,0x08,0x50,0xC3,0x29,0x54,0x0D,0xA6,0x3E,0x66,0xD1,
		0xC0,0x16,0xA8,0xAD,0x2A,0x66,0x1E,0x97,0xCB,0xBB,0x05,0xBE,
		0x70,0x09,0xD2,0x82,0x80,0x34,0x6E,0xB5,0x20,0xF7,0x11,0x1E,
		0xAE,0xD4,0x8C,0xEB,0x2F,0x94,0x0E,0xB3,0xA5,0x07,0xD0,0xA3,
		0x29,0x07,0x40,0x29,0x87,0x57,0x96,0xBB,0x7F,0x47,0x5D,0xD9,
		0x03,0x28,0x88,0xDE,0x22,0x61,0x33,0xB3,0xD1,0x8A,0xA8,0x74,
		0x85,0x70,0x2B,0xF4,0x9B,0xA9,0x31,0xC4,0x61,0xF6,0xE4,0x4C,
		0xFD,0x29,0xA0,0xA9,0x7A,0xF8,0xF3,0xEA,0xAF,0x13,0x5C,0xF2,
		0xFB,0x01,0x2B,0xA8,0xA9,0x12,0xC6,0xCC,0xDE,0x0E,0x1D,0xF7,
		0x26,0xFC,0x86,0x05,0x17,0xA4,0xE1,0x7E,0xFD,0xB6,0x48,0x95,
		0x8B,0x80,0x38,0x05,0xD2,0x7B,0x8E,0xC7,0x20,0xD0,0xAD,0x43,
		0xF5,0x91,0x70,0xF5,0x58,0x0F,0x91,0xAF,0xED,0x5B,0xEC,0x76,
		0x1F,0x64,0x1E,0xAC,0x57,0xE0,0x9A,0x0C,0x01,0xC3,0xF8,0xDF,
		0xEE,0xD1,0x5D,0xA7,0x55,0xE2,0x9F,0xD6,0x30,0x7E,0x22,0xCB,
		0x85,0x47,0xF2,0x2B,0x62,0x91,0xAE,0x43,0x10,0x32,0xD6,0x87,
		0x7B,0xA1,0x6D,0xD0,0x4B,0x27,0x72,0xFD,0x7D,0x79,0x70,0x60,
		0xF4,0x18,0xD3,0xDF,0x44,0xB9,0xD4,0x84,0xB1,0x2A,0xBF,0x7C,
		0xDA,0x9E,0xFE,0x9F,0xFD,0xBD,0x8E,0x9C,0x88,0xB0,0x22,0x5D,
		0xCF,0xDD,0xF7,0xEB,0x96,0x02,0xA7,0x3A,0xC0,0xB3,0x92,0xD3,
		0xD8,0xE6,0x44,0xB2,0xF8,0x96,0xEE,0x68,0x4B,0x89,0x78,0x4F,
		0x1C,0x95,0xA9,0xDE,0x24,0x56,0xF0,0x7A,0xA4,0xB7,0x05,0xE7,
		0x46,0xAA,0xF1,0x51,0xB9,0x08,0xCD,0x72,0x74,0xD9,0xA0,0x06,
		0xFC,0x70,0xFD,0xD2,0xFF,0xC3,0x47,0xB8,0x00,0x6A,0xED,0x3C,
		0xBA,0x1E,0x9C,0x43,0x92,0xE1,0x14,0x32,0x54,0x21,0x29,0xC3,
		0xB7,0xEB,0xFF,0x87,0x9D,0xB5,0x3D,0x04,0x33,0x56,0xA9,0xBA,
		0x27,0xB7,0x2E,0x05,0x7A,0x87,0xA3,0xC7,0xB9,0xAE,0xC6,0x57,
		0xDB,0xAA,0xB1,0x01,0xC2,0x0D,0xB7,0x9F,0x0C,0x13,
		};
	static const unsigned char dh3247_g[]={
		0x33,0xE0,0x1D,0x9B,0x83,0xB5,0x7B,0x26,0x0A,0x63,0x34,0x3B,
		0x4E,0x49,0x52,0xC8,0x1E,0xBD,0x78,0x50,0x6C,0x1D,0x5F,0xA1,
		0xAC,0x70,0xF9,0xD6,0xA3,0x39,0x23,0xF5,0x79,0xEC,0xFF,0x84,
		0xB5,0x8F,0x65,0xA3,0xF5,0xB8,0x7F,0x21,0x4A,0x3E,0x40,0x4E,
		0x96,0x77,0x34,0xEC,0x28,0x2C,0x43,0x86,0x7A,0x56,0x75,0x4C,
		0x32,0xDA,0x75,0xFD,0xE4,0x51,0x8D,0x4F,0x3B,0xFC,0x2D,0xC4,
		0x95,0xD6,0xBA,0xF5,0x19,0x9A,0xC4,0x76,0x88,0x79,0x0F,0xB3,
		0x50,0x17,0xBC,0x5D,0xCB,0x7B,0x2A,0x28,0x11,0x36,0x68,0x28,
		0x1B,0x4A,0xDC,0xE4,0x76,0x51,0x69,0xD8,0x81,0xAF,0xBA,0x75,
		0x8C,0xA8,0xB9,0x5F,0x9A,0x00,0xE7,0xB0,0x3C,0xEC,0x40,0xB8,
		0xB3,0x49,0x4C,0x8B,0xD7,0xC0,0x9A,0x95,0x1A,0xFA,0xC6,0x47,
		0x9A,0x46,0xC1,0xDC,0x0D,0x7C,0x7C,0x05,0x60,0xBD,0xF5,0xFF,
		0x28,0x09,0x9F,0x9F,0x81,0xEE,0x62,0x38,0x68,0xDF,0x55,0xDB,
		0xA5,0x1D,0x3D,0x54,0x54,0x1A,0xA4,0xF1,0x4A,0xD0,0x3F,0x6C,
		0x55,0xF5,0x3A,0x7D,0x73,0x05,0x14,0xAC,0x45,0xD7,0xF4,0x24,
		0x25,0xA9,0x82,0xDF,0xD4,0x11,0x17,0x90,0xD6,0x22,0x8A,0x5A,
		0x34,0x38,0xCD,0x89,0xB9,0xCE,0xBF,0x6F,0x37,0xA4,0xFE,0x87,
		0xE4,0xF4,0x9D,0x9C,0xE4,0xCA,0x3C,0x9D,0x1D,0x8D,0xB2,0xE9,
		0x85,0xA4,0x6F,0x84,0x4E,0x21,0x22,0xB6,0xDF,0xD1,0xE6,0x3D,
		0x3B,0xA3,0xCD,0x56,0x74,0x05,0x06,0xF2,0x3D,0x89,0x00,0xF9,
		0x1F,0x5E,0xCE,0x09,0xEB,0x88,0x70,0x02,0x0C,0x1A,0xB5,0x15,
		0x4B,0x02,0x14,0xD7,0x01,0x56,0x0F,0xDE,0xDA,0xB3,0xA0,0xDF,
		0x69,0x2D,0x8A,0x7A,0x20,0x5C,0x4B,0xBC,0xE5,0x48,0x33,0xA2,
		0x3F,0x87,0x6C,0xAC,0xE0,0xFB,0x59,0x21,0x7A,0xD6,0x62,0x3C,
		0xDD,0x6B,0xD0,0xA0,0x5B,0xF1,0x7E,0xD6,0x45,0xA7,0xED,0xCD,
		0x0D,0x6F,0x03,0x6D,0x4B,0xE1,0x58,0x94,0xE8,0xCE,0xDC,0xAC,
		0xB3,0x66,0x1D,0x40,0xE6,0x3D,0x3E,0x7F,0xA6,0x39,0xEE,0x22,
		0x48,0x2D,0x65,0x34,0x3B,0x49,0xCE,0x74,0xB9,0xE6,0x57,0xD9,
		0x0F,0xB3,0xA6,0xAB,0x09,0x16,0x8B,0x95,0x7A,0x35,0xB4,0xD9,
		0xE4,0xEF,0x91,0x78,0x7F,0x8A,0xA0,0xCD,0xC4,0x32,0x4A,0xD0,
		0xB6,0x93,0xCA,0x37,0xDD,0x7D,0x2C,0x8D,0x94,0x8E,0x0F,0xF9,
		0x65,0xF2,0x4D,0x1A,0x26,0x7D,0x63,0x98,0xA6,0x1A,0x7B,0xEE,
		0x59,0x5F,0x46,0x32,0xB2,0xB7,0xB3,0x27,0x93,0xBA,0x3A,0xBD,
		0x10,0x56,0x9F,0xA1,0xC8,0x69,0xDB,0x9A,0x69,0x6F,
		};
	static const gnutls_datum_t prime = { (unsigned char*) dh3247_p, sizeof(dh3247_p) };
	static const gnutls_datum_t generator = { (unsigned char*) dh3247_g, sizeof(dh3247_g) };
	int r;
	gnutls_dh_params_t params;

	if (GNUTLS_E_SUCCESS != (r = gnutls_dh_params_init(&params))) return r;
	if (GNUTLS_E_SUCCESS != (r = gnutls_dh_params_import_raw(params, &prime, &generator))) {
		gnutls_dh_params_deinit(params);
		return r;
	}
	*dh_params = params;
	return GNUTLS_E_SUCCESS;
}

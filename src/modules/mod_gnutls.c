/*
 * mod_gnutls - TLS listen support
 *
 * TODO:
 *   * support client certificate authentication: https://www.gnutls.org/manual/gnutls.html#Client-certificate-authentication
 *     gnutls_certificate_set_x509_system_trust (available since 3.0 (docs) or 3.0.19 (weechat ??))
 *     gnutls_certificate_set_x509_trust_file
 *   * TLS session tickets are always activated with gnutls >= 2.10 - option to disable
 *   * OCSP stapling
 *
 * Author:
 *     Copyright (c) 2013 Stefan Bühler
 */

#include <lighttpd/base.h>
#include <lighttpd/throttle.h>

#include "gnutls_filter.h"
#include "gnutls_ocsp.h"
#include "ssl_client_hello_parser.h"
#include "ssl-session-db.h"

#include <gnutls/gnutls.h>

#if GNUTLS_VERSION_NUMBER >= 0x020a00
#define HAVE_SESSION_TICKET
#endif

#if GNUTLS_VERSION_NUMBER >= 0x03010b
#define HAVE_PIN
#endif

LI_API gboolean mod_gnutls_init(liModules *mods, liModule *mod);
LI_API gboolean mod_gnutls_free(liModules *mods, liModule *mod);

static int load_dh_params_4096(gnutls_dh_params_t *dh_params);

typedef struct mod_connection_ctx mod_connection_ctx;
typedef struct mod_context mod_context;

struct mod_connection_ctx {
	gnutls_session_t session;
	liConnection *con;
	mod_context *ctx;

	liGnuTLSFilter *tls_filter;

	liIOStream *sock_stream;
	gpointer simple_socket_data;

	liStream *client_hello_stream;
#ifdef USE_SNI
	liJob sni_job;
	liJobRef *sni_jobref;
	liFetchEntry *sni_entry;
	liFetchWait *sni_db_wait;
	GString *sni_server_name;
#endif
};

struct mod_context {
	gint refcount;

	liServer *srv;

	liSSLSessionDB *session_db;

	gnutls_certificate_credentials_t server_cert;
	gnutls_dh_params_t dh_params;
	gnutls_priority_t server_priority;
	gnutls_priority_t server_priority_beast;
#ifdef HAVE_SESSION_TICKET
	gnutls_datum_t ticket_key;
#endif

	liGnuTLSOCSP* ocsp;

#ifdef USE_SNI
	liFetchDatabase *sni_db, *sni_backend_db;
	gnutls_certificate_credentials_t sni_fallback_cert;
#endif
	GString *pin;

	unsigned int protect_against_beast:1;
};

#ifdef USE_SNI
typedef struct fetch_cert_backend_lookup fetch_cert_backend_lookup;
struct fetch_cert_backend_lookup {
	liFetchWait *wait;
	liFetchEntry *entry;
	mod_context *ctx;
};

typedef struct sni_cert_data sni_cert_data;
struct sni_cert_data {
	gnutls_certificate_credentials_t creds;
	liGnuTLSOCSP* ocsp;
};
#endif

static void mod_gnutls_context_release(mod_context *ctx);
static void mod_gnutls_context_acquire(mod_context *ctx);

#if defined(HAVE_PIN)
static int pin_callback(void *user, int attempt, const char *token_url, const char *token_label, unsigned int flags, char *pin, size_t pin_max) {
	GString *pinString = user;
	size_t saved_pin_len;
	UNUSED(flags);
	UNUSED(token_url);
	UNUSED(token_label);

	if (NULL == pinString) return -1;

	if (0 != attempt) return -1;

	/* include terminating 0 */
	saved_pin_len = 1 + pinString->len;
	if (saved_pin_len > pin_max) return -1;

	memcpy(pin, pinString->str, saved_pin_len);
	return 0;
}
#endif /* defined(HAVE_PIN) */

#ifdef USE_SNI
static sni_cert_data* creds_from_gstring(mod_context *ctx, GString *str) {
	sni_cert_data* data = NULL;
	gnutls_certificate_credentials_t creds = NULL;
	liGnuTLSOCSP* ocsp = NULL;
	gnutls_datum_t pemfile;
	int r;

	if (NULL == str) return NULL;

	if (GNUTLS_E_SUCCESS > (r = gnutls_certificate_allocate_credentials(&creds))) return NULL;

	pemfile.data = (unsigned char*) str->str;
	pemfile.size = str->len;

#if defined(HAVE_PIN)
	gnutls_certificate_set_pin_function(creds, pin_callback, ctx->pin);
#endif

	if (GNUTLS_E_SUCCESS > (r = gnutls_certificate_set_x509_key_mem(creds, &pemfile, &pemfile, GNUTLS_X509_FMT_PEM))) {
		goto error;
	}

	gnutls_certificate_set_dh_params(creds, ctx->dh_params);

	ocsp = li_gnutls_ocsp_new();
	if (!li_gnutls_ocsp_search_datum(ctx->srv, ocsp, &pemfile)) {
		goto error;
	}
	li_gnutls_ocsp_use(ocsp, creds);

	data = g_slice_new0(sni_cert_data);
	data->creds = creds;
	data->ocsp = ocsp;

	return data;

error:
	if (NULL != creds) gnutls_certificate_free_credentials(creds);
	if (NULL != ocsp) li_gnutls_ocsp_free(ocsp);
	return NULL;
}

static void fetch_cert_backend_ready(gpointer data) {
	liFetchEntry *be;
	fetch_cert_backend_lookup *lookup = (fetch_cert_backend_lookup*) data;

	be = li_fetch_get2(lookup->ctx->sni_backend_db, lookup->entry->key, fetch_cert_backend_ready, lookup, &lookup->wait);
	if (NULL != be) {
		liFetchEntry *entry = lookup->entry;
		mod_context *ctx = lookup->ctx;
		g_slice_free(fetch_cert_backend_lookup, lookup);

		entry->backend_data = be;
		entry->data = creds_from_gstring(ctx, (GString*) be->data);
		li_fetch_entry_ready(entry);
	}
}

static void fetch_cert_lookup(liFetchDatabase* db, gpointer data, liFetchEntry *entry) {
	fetch_cert_backend_lookup *lookup = g_slice_new0(fetch_cert_backend_lookup);
	UNUSED(db);

	lookup->ctx = (mod_context*) data;
	lookup->entry = entry;
	fetch_cert_backend_ready(lookup);
}
static gboolean fetch_cert_revalidate(liFetchDatabase* db, gpointer data, liFetchEntry *entry) {
	UNUSED(db); UNUSED(data);
	return li_fetch_entry_revalidate((liFetchEntry*) entry->backend_data);
}
static void fetch_cert_refresh(liFetchDatabase* db, gpointer data, liFetchEntry *cur_entry, liFetchEntry *new_entry) {
	UNUSED(db); UNUSED(data);
	li_fetch_entry_refresh((liFetchEntry*) cur_entry->backend_data);
	li_fetch_entry_refresh_skip(new_entry);
}
static void fetch_cert_free_entry(gpointer data, liFetchEntry *entry) {
	sni_cert_data* sni_data = entry->data;
	UNUSED(data);

	if (NULL != sni_data) {
		if (NULL != sni_data->creds) gnutls_certificate_free_credentials(sni_data->creds);
		if (NULL != sni_data->ocsp) li_gnutls_ocsp_free(sni_data->ocsp);
		g_slice_free(sni_cert_data, sni_data);
	}
	li_fetch_entry_release((liFetchEntry*) entry->backend_data);
}
static void fetch_cert_free_db(gpointer data) {
	mod_context *ctx = (mod_context*) data;
	mod_gnutls_context_release(ctx);
}


static const liFetchCallbacks fetch_cert_callbacks = {
	fetch_cert_lookup,
	fetch_cert_revalidate,
	fetch_cert_refresh,
	fetch_cert_free_entry,
	fetch_cert_free_db
};
#endif


static void mod_gnutls_context_release(mod_context *ctx) {
	if (!ctx) return;
	LI_FORCE_ASSERT(g_atomic_int_get(&ctx->refcount) > 0);
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
		li_ssl_session_db_free(ctx->session_db);
		ctx->session_db = NULL;

#ifdef USE_SNI
		if (NULL != ctx->sni_db) {
			li_fetch_database_release(ctx->sni_db);
			ctx->sni_db = NULL;
		}
		if (NULL != ctx->sni_backend_db) {
			li_fetch_database_release(ctx->sni_backend_db);
			ctx->sni_backend_db = NULL;
		}
		if (NULL != ctx->sni_fallback_cert) {
			gnutls_certificate_free_credentials(ctx->sni_fallback_cert);
			ctx->sni_fallback_cert = NULL;
		}
#endif
		gnutls_dh_params_deinit(ctx->dh_params);

		if (NULL != ctx->pin) {
			g_string_free(ctx->pin, TRUE);
			ctx->pin = NULL;
		}

		li_gnutls_ocsp_free(ctx->ocsp);

		g_slice_free(mod_context, ctx);
	}
}

static void mod_gnutls_context_acquire(mod_context *ctx) {
	LI_FORCE_ASSERT(g_atomic_int_get(&ctx->refcount) > 0);
	g_atomic_int_inc(&ctx->refcount);
}


static mod_context *mod_gnutls_context_new(liServer *srv) {
	mod_context *ctx = g_slice_new0(mod_context);
	int r;

	if (GNUTLS_E_SUCCESS > (r = gnutls_certificate_allocate_credentials(&ctx->server_cert))) {
		ERROR(srv, "gnutls_certificate_allocate_credentials failed(%s): %s",
			gnutls_strerror_name(r), gnutls_strerror(r));
		goto error0;
	}

	if (GNUTLS_E_SUCCESS > (r = gnutls_priority_init(&ctx->server_priority, "NORMAL", NULL))) {
		ERROR(srv, "gnutls_priority_init('NORMAL') failed(%s): %s",
			gnutls_strerror_name(r), gnutls_strerror(r));
		goto error1;
	}

	if (GNUTLS_E_SUCCESS > (r = gnutls_priority_init(&ctx->server_priority_beast, "NORMAL:-CIPHER-ALL:+ARCFOUR-128", NULL))) {
		int r1;
		if (GNUTLS_E_SUCCESS > (r1 = gnutls_priority_init(&ctx->server_priority_beast, "NORMAL", NULL))) {
			ERROR(srv, "gnutls_priority_init('NORMAL') failed(%s): %s",
				gnutls_strerror_name(r1), gnutls_strerror(r1));
			goto error2;
		} else {
			ERROR(srv, "gnutls_priority_init('NORMAL:-CIPHER-ALL:+ARCFOUR-128') failed(%s): %s. Using 'NORMAL' instead (BEAST mitigation not available)",
				gnutls_strerror_name(r), gnutls_strerror(r));
		}
	}

#ifdef HAVE_SESSION_TICKET
	if (GNUTLS_E_SUCCESS > (r = gnutls_session_ticket_key_generate(&ctx->ticket_key))) {
		ERROR(srv, "gnutls_session_ticket_key_generate failed(%s): %s",
			gnutls_strerror_name(r), gnutls_strerror(r));
		goto error3;
	}
#endif

	ctx->srv = srv;

	ctx->ocsp = li_gnutls_ocsp_new();

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
	LI_FORCE_ASSERT(NULL == conctx->sock_stream || conctx->sock_stream == stream);

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
		LI_FORCE_ASSERT(NULL == conctx->sock_stream);
		LI_FORCE_ASSERT(NULL == conctx->tls_filter);
		LI_FORCE_ASSERT(NULL == conctx->con);
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
	LI_FORCE_ASSERT(conctx->tls_filter == f);

	conctx->tls_filter = NULL;
	li_gnutls_filter_free(f);
	gnutls_deinit(conctx->session);

	if (NULL != conctx->ctx) {
		mod_gnutls_context_release(conctx->ctx);
		conctx->ctx = NULL;
	}

	if (NULL != conctx->con) {
		liStream *raw_out = con->con_sock.raw_out, *raw_in = con->con_sock.raw_in;
		LI_FORCE_ASSERT(con->con_sock.data == conctx);
		conctx->con = NULL;
		con->con_sock.data = NULL;
		con->con_sock.callbacks = NULL;
		li_stream_acquire(raw_in);
		li_stream_reset(raw_out);
		li_stream_reset(raw_in);
		li_stream_release(raw_in);
	}

#ifdef USE_SNI
	if (NULL != conctx->sni_db_wait) {
		li_fetch_cancel(&conctx->sni_db_wait);
	}
	if (NULL != conctx->sni_entry) {
		li_fetch_entry_release(conctx->sni_entry);
		conctx->sni_entry = NULL;
	}
#endif

	if (NULL != conctx->client_hello_stream) {
		li_ssl_client_hello_stream_ready(conctx->client_hello_stream);
		li_stream_release(conctx->client_hello_stream);
		conctx->client_hello_stream = NULL;
	}

#ifdef USE_SNI
	if (NULL != conctx->sni_jobref) {
		li_job_ref_release(conctx->sni_jobref);
		conctx->sni_jobref = NULL;
	}
	li_job_clear(&conctx->sni_job);

	if (NULL != conctx->sni_server_name) {
		g_string_free(conctx->sni_server_name, TRUE);
		conctx->sni_server_name = NULL;
	}
#endif

	LI_FORCE_ASSERT(NULL != conctx->sock_stream);
	li_iostream_safe_release(&conctx->sock_stream);
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
	NULL
};

static void gnutls_tcp_finished(liConnection *con, gboolean aborted) {
	mod_connection_ctx *conctx = con->con_sock.data;
	UNUSED(aborted);

	con->info.is_ssl = FALSE;
	con->con_sock.callbacks = NULL;

	if (NULL != conctx) {
		LI_FORCE_ASSERT(con == conctx->con);
		close_cb(conctx->tls_filter, conctx);
		LI_FORCE_ASSERT(NULL == con->con_sock.data);
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

#ifdef USE_SNI
static void sni_job_cb(liJob *job) {
	mod_connection_ctx *conctx = LI_CONTAINER_OF(job, mod_connection_ctx, sni_job);

	LI_FORCE_ASSERT(NULL != conctx->client_hello_stream);

	conctx->sni_entry = li_fetch_get(conctx->ctx->sni_db, conctx->sni_server_name, conctx->sni_jobref, &conctx->sni_db_wait);
	if (conctx->sni_entry != NULL) {
		sni_cert_data* data = conctx->sni_entry->data;
		if (NULL != data) {
			gnutls_credentials_set(conctx->session, GNUTLS_CRD_CERTIFICATE, data->creds);
		} else if (NULL != conctx->ctx->sni_fallback_cert) {
			gnutls_credentials_set(conctx->session, GNUTLS_CRD_CERTIFICATE, conctx->ctx->sni_fallback_cert);
		}
		li_ssl_client_hello_stream_ready(conctx->client_hello_stream);
	}
}
#endif

static void gnutls_client_hello_cb(gpointer data, gboolean success, GString *server_name, guint16 protocol) {
	mod_connection_ctx *conctx = data;

	if (conctx->ctx->protect_against_beast) {
		if (!success || protocol <= 0x0301) { /* SSL3: 0x0300, TLS1.0: 0x0301 */
			gnutls_priority_set(conctx->session, conctx->ctx->server_priority_beast);
		}
	}

#ifdef USE_SNI
	if (success && NULL != server_name && NULL != conctx->ctx->sni_db && server_name->len > 0) {
		conctx->sni_server_name = g_string_new_len(GSTR_LEN(server_name));
		sni_job_cb(&conctx->sni_job);
		return;
	}
#else
	UNUSED(server_name);
#endif

	li_ssl_client_hello_stream_ready(conctx->client_hello_stream);
}

static gboolean mod_gnutls_con_new(liConnection *con, int fd) {
	liEventLoop *loop = &con->wrk->loop;
	liServer *srv = con->srv;
	mod_context *ctx = con->srv_sock->data;
	mod_connection_ctx *conctx;
	gnutls_session_t session;
	int r;

	if (GNUTLS_E_SUCCESS > (r = gnutls_init(&session, GNUTLS_SERVER))) {
		ERROR(srv, "gnutls_init (%s): %s",
			gnutls_strerror_name(r), gnutls_strerror(r));
		return FALSE;
	}

	mod_gnutls_context_acquire(ctx);

	if (GNUTLS_E_SUCCESS > (r = gnutls_priority_set(session, ctx->server_priority))) {
		ERROR(srv, "gnutls_priority_set (%s): %s",
			gnutls_strerror_name(r), gnutls_strerror(r));
		goto fail;
	}
	if (GNUTLS_E_SUCCESS > (r = gnutls_credentials_set(session, GNUTLS_CRD_CERTIFICATE, ctx->server_cert))) {
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
	if (GNUTLS_E_SUCCESS > (r = gnutls_session_ticket_enable_server(session, &ctx->ticket_key))) {
		ERROR(srv, "gnutls_session_ticket_enable_server (%s): %s",
			gnutls_strerror_name(r), gnutls_strerror(r));
		goto fail;
	}
#endif

#ifdef GNUTLS_ALPN_MAND
	{
		static const gnutls_datum_t proto_http1 = { (unsigned char*) CONST_STR_LEN("http/1.1") };
		gnutls_alpn_set_protocols(session, &proto_http1, 1, 0);
	}
#endif

	conctx = g_slice_new0(mod_connection_ctx);
	conctx->session = session;
	conctx->sock_stream = li_iostream_new(con->wrk, fd, tcp_io_cb, conctx);

	conctx->client_hello_stream = li_ssl_client_hello_stream(&con->wrk->loop, gnutls_client_hello_cb, conctx);
#ifdef USE_SNI
	li_job_init(&conctx->sni_job, sni_job_cb);
	conctx->sni_jobref = li_job_ref(&con->wrk->loop.jobqueue, &conctx->sni_job);
#endif

	li_stream_connect(&conctx->sock_stream->stream_in, conctx->client_hello_stream);

	conctx->tls_filter = li_gnutls_filter_new(srv, con->wrk, &filter_callbacks, conctx, conctx->session,
		conctx->client_hello_stream, &conctx->sock_stream->stream_out);

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

static gboolean creds_add_pemfile(liServer *srv, mod_context *ctx, gnutls_certificate_credentials_t creds, liValue *pemfile) {
	const char *keyfile = NULL;
	const char *certfile = NULL;
	const char *ocspfile = NULL;
	int r;

	if (LI_VALUE_STRING == li_value_type(pemfile)) {
		keyfile = pemfile->data.string->str;
		certfile = pemfile->data.string->str;
	} else if (li_value_list_len(pemfile) >= 2) {
		if (NULL == (pemfile = li_value_to_key_value_list(pemfile))) {
			ERROR(srv, "%s", "gnutls expects a hash/key-value list or a string as pemfile parameter");
			return FALSE;
		}

		LI_VALUE_FOREACH(entry, pemfile)
			liValue *entryKey = li_value_list_at(entry, 0);
			liValue *entryValue = li_value_list_at(entry, 1);
			GString *entryKeyStr;

			if (LI_VALUE_STRING != li_value_type(entryKey)) {
				ERROR(srv, "%s", "gnutls doesn't take default keys");
				return FALSE;
			}
			entryKeyStr = entryKey->data.string; /* keys are either NONE or STRING */

			if (g_str_equal(entryKeyStr->str, "key")) {
				if (LI_VALUE_STRING != li_value_type(entryValue)) {
					ERROR(srv, "%s", "gnutls pemfile.key expects a string as parameter");
					return FALSE;
				}
				if (NULL != keyfile) {
					ERROR(srv, "gnutls unexpected duplicate parameter pemfile %s", entryKeyStr->str);
					return FALSE;
				}
				keyfile = entryValue->data.string->str;
			} else if (g_str_equal(entryKeyStr->str, "cert")) {
				if (LI_VALUE_STRING != li_value_type(entryValue)) {
					ERROR(srv, "%s", "gnutls pemfile.cert expects a string as parameter");
					return FALSE;
				}
				if (NULL != certfile) {
					ERROR(srv, "gnutls unexpected duplicate parameter pemfile %s", entryKeyStr->str);
					return FALSE;
				}
				certfile = entryValue->data.string->str;
			} else if (g_str_equal(entryKeyStr->str, "ocsp")) {
				if (LI_VALUE_STRING != li_value_type(entryValue)) {
					ERROR(srv, "%s", "gnutls pemfile.ocsp expects a string as parameter");
					return FALSE;
				}
				if (NULL != ocspfile) {
					ERROR(srv, "gnutls unexpected duplicate parameter pemfile %s", entryKeyStr->str);
					return FALSE;
				}
				ocspfile = entryValue->data.string->str;
			} else {
				ERROR(srv, "invalid parameter for gnutls: pemfile %s", entryKeyStr->str);
				return FALSE;
			}
		LI_VALUE_END_FOREACH()
	} else {
		ERROR(srv, "%s", "gnutls expects a hash/key-value list (with at least \"key\" and \"cert\" entries) or a string as pemfile parameter");
		return FALSE;
	}

	if (NULL == keyfile || NULL == certfile) {
		ERROR(srv, "%s", "gnutls: missing key or cert in pemfile parameter");
		return FALSE;
	}

	if (GNUTLS_E_SUCCESS > (r = gnutls_certificate_set_x509_key_file(creds, certfile, keyfile, GNUTLS_X509_FMT_PEM))) {
		ERROR(srv, "gnutls_certificate_set_x509_key_file failed(certfile '%s', keyfile '%s', PEM) (%s): %s",
			certfile, keyfile,
			gnutls_strerror_name(r), gnutls_strerror(r));
		return FALSE;
	}

	if (NULL != ocspfile) {
		if (!li_gnutls_ocsp_add(srv, ctx->ocsp, ocspfile)) return FALSE;
	} else {
		if (!li_gnutls_ocsp_search(srv, ctx->ocsp, certfile)) return FALSE;
	}

	return TRUE;
}

static gboolean gnutls_setup(liServer *srv, liPlugin* p, liValue *val, gpointer userdata) {
	mod_context *ctx;
	int r;

	/* setup defaults */
	gboolean have_listen_parameter = FALSE;
	gboolean have_pemfile_parameter = FALSE;
	gboolean have_protect_beast_parameter = FALSE;
	gboolean have_session_db_size_parameter = FALSE;
	const char *priority = NULL, *dh_params_file = NULL;
#ifdef USE_SNI
	const char *sni_backend = NULL;
	liValue *sni_fallback_pemfile = NULL;
#endif
	gboolean protect_against_beast = FALSE;
	gint64 session_db_size = 256;
#if defined(HAVE_PIN)
	liValue *pin = NULL;
#endif

	UNUSED(p); UNUSED(userdata);

	val = li_value_get_single_argument(val);

	if (NULL == (val = li_value_to_key_value_list(val))) {
		ERROR(srv, "%s", "gnutls expects a hash/key-value list as parameter");
		return FALSE;
	}

	LI_VALUE_FOREACH(entry, val)
		liValue *entryKey = li_value_list_at(entry, 0);
		liValue *entryValue = li_value_list_at(entry, 1);
		GString *entryKeyStr;

		if (LI_VALUE_STRING != li_value_type(entryKey)) {
			ERROR(srv, "%s", "gnutls doesn't take default keys");
			return FALSE;
		}
		entryKeyStr = entryKey->data.string; /* keys are either NONE or STRING */

		if (g_str_equal(entryKeyStr->str, "listen")) {
			if (LI_VALUE_STRING != li_value_type(entryValue)) {
				ERROR(srv, "%s", "gnutls listen expects a string as parameter");
				return FALSE;
			}
			have_listen_parameter = TRUE;
		} else if (g_str_equal(entryKeyStr->str, "pemfile")) {
			/* check type later */
			have_pemfile_parameter = TRUE;
		} else if (g_str_equal(entryKeyStr->str, "dh-params")) {
			if (LI_VALUE_STRING != li_value_type(entryValue)) {
				ERROR(srv, "%s", "gnutls dh-params expects a string as parameter");
				return FALSE;
			}
			if (NULL != dh_params_file) {
				ERROR(srv, "gnutls unexpected duplicate parameter %s", entryKeyStr->str);
				return FALSE;
			}
			dh_params_file = entryValue->data.string->str;
		} else if (g_str_equal(entryKeyStr->str, "priority")) {
			if (LI_VALUE_STRING != li_value_type(entryValue)) {
				ERROR(srv, "%s", "gnutls priority expects a string as parameter");
				return FALSE;
			}
			if (NULL != priority) {
				ERROR(srv, "gnutls unexpected duplicate parameter %s", entryKeyStr->str);
				return FALSE;
			}
			priority = entryValue->data.string->str;
		} else if (g_str_equal(entryKeyStr->str, "pin")) {
#if defined(HAVE_PIN)
			if (LI_VALUE_STRING != li_value_type(entryValue)) {
				ERROR(srv, "%s", "gnutls pin expects a string as parameter");
				return FALSE;
			}
			if (NULL != pin) {
				ERROR(srv, "gnutls unexpected duplicate parameter %s", entryKeyStr->str);
				return FALSE;
			}
			pin = entryValue;
#else
			ERROR(srv, "%s", "mod_gnutls: pin not supported; compile with gnutls >= 3.1.11");
			return FALSE;
#endif
		} else if (g_str_equal(entryKeyStr->str, "protect-against-beast")) {
			if (LI_VALUE_BOOLEAN != li_value_type(entryValue)) {
				ERROR(srv, "%s", "gnutls protect-against-beast expects a boolean as parameter");
				return FALSE;
			}
			if (have_protect_beast_parameter) {
				ERROR(srv, "gnutls unexpected duplicate parameter %s", entryKeyStr->str);
				return FALSE;
			}
			have_protect_beast_parameter = TRUE;
			protect_against_beast = entryValue->data.boolean;
		} else if (g_str_equal(entryKeyStr->str, "session-db-size")) {
			if (LI_VALUE_NUMBER != li_value_type(entryValue)) {
				ERROR(srv, "%s", "gnutls session-db-size expects an integer as parameter");
				return FALSE;
			}
			if (have_session_db_size_parameter) {
				ERROR(srv, "gnutls unexpected duplicate parameter %s", entryKeyStr->str);
				return FALSE;
			}
			have_session_db_size_parameter = TRUE;
			session_db_size = entryValue->data.number;
#ifdef USE_SNI
		} else if (g_str_equal(entryKeyStr->str, "sni-backend")) {
			if (LI_VALUE_STRING != li_value_type(entryValue)) {
				ERROR(srv, "%s", "gnutls sni-backend expects a string as parameter");
				return FALSE;
			}
			if (NULL != sni_backend) {
				ERROR(srv, "gnutls unexpected duplicate parameter %s", entryKeyStr->str);
				return FALSE;
			}
			sni_backend = entryValue->data.string->str;
		} else if (g_str_equal(entryKeyStr->str, "sni-fallback-pemfile")) {
			/* check type later */
			if (NULL != sni_fallback_pemfile) {
				ERROR(srv, "gnutls unexpected duplicate parameter %s", entryKeyStr->str);
				return FALSE;
			}
			sni_fallback_pemfile = entryValue;
#else
		} else if (g_str_equal(entryKeyStr->str, "sni-backend")) {
			ERROR(srv, "%s", "mod_gnutls was build without SNI support, invalid option sni-backend");
			return FALSE;
		} else if (g_str_equal(entryKeyStr->str, "sni-fallback-pemfile")) {
			ERROR(srv, "%s", "mod_gnutls was build without SNI support, invalid option gnutls sni-fallback-pemfile");
			return FALSE;
#endif
		} else {
			ERROR(srv, "invalid parameter for gnutls: %s", entryKeyStr->str);
			return FALSE;
		}
	LI_VALUE_END_FOREACH()

	if (!have_listen_parameter) {
		ERROR(srv, "%s", "gnutls needs a listen parameter");
		return FALSE;
	}

	if (!have_pemfile_parameter) {
		ERROR(srv, "%s", "gnutls needs a pemfile");
		return FALSE;
	}

	if (!(ctx = mod_gnutls_context_new(srv))) return FALSE;

	ctx->protect_against_beast = protect_against_beast;

#if defined(HAVE_PIN)
	ctx->pin = li_value_extract_string(pin);
	gnutls_certificate_set_pin_function(ctx->server_cert, pin_callback, ctx->pin);
#endif

	li_gnutls_ocsp_use(ctx->ocsp, ctx->server_cert);

#ifdef USE_SNI
	if (NULL != sni_backend) {
		liFetchDatabase *backend = li_server_get_fetch_database(srv, sni_backend);
		if (NULL == backend) {
			ERROR(srv, "gnutls: no fetch backend with name '%s' registered", sni_backend);
			goto error_free_ctx;
		}
		ctx->sni_backend_db = backend;
		mod_gnutls_context_acquire(ctx);
		ctx->sni_db = li_fetch_database_new(&fetch_cert_callbacks, ctx, 64, 16);
	}

	if (NULL != sni_fallback_pemfile) {
		if (GNUTLS_E_SUCCESS > (r = gnutls_certificate_allocate_credentials(&ctx->sni_fallback_cert))) {
			ERROR(srv, "gnutls_certificate_allocate_credentials failed(%s): %s",
				gnutls_strerror_name(r), gnutls_strerror(r));
			goto error_free_ctx;
		}

#if defined(HAVE_PIN)
		gnutls_certificate_set_pin_function(ctx->sni_fallback_cert, pin_callback, ctx->pin);
#endif

		li_gnutls_ocsp_use(ctx->ocsp, ctx->sni_fallback_cert);

		if (!creds_add_pemfile(srv, ctx, ctx->sni_fallback_cert, sni_fallback_pemfile)) {
			goto error_free_ctx;
		}
	}
#endif

	LI_VALUE_FOREACH(entry, val)
		liValue *entryKey = li_value_list_at(entry, 0);
		liValue *entryValue = li_value_list_at(entry, 1);
		GString *entryKeyStr;

		if (LI_VALUE_STRING != li_value_type(entryKey)) continue;
		entryKeyStr = entryKey->data.string; /* keys are either NONE or STRING */

		if (g_str_equal(entryKeyStr->str, "pemfile")) {

			if (!creds_add_pemfile(srv, ctx, ctx->server_cert, entryValue)) {
				goto error_free_ctx;
			}
		}
	LI_VALUE_END_FOREACH()

	if (session_db_size > 0) ctx->session_db = li_ssl_session_db_new(session_db_size);

	if (NULL != dh_params_file) {
		gchar *contents = NULL;
		gsize length = 0;
		GError *error = NULL;
		gnutls_datum_t pkcs3_params;

		if (GNUTLS_E_SUCCESS > (r = gnutls_dh_params_init(&ctx->dh_params))) {
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

		if (GNUTLS_E_SUCCESS > r) {
			ERROR(srv, "couldn't load dh parameters from file '%s' (%s): %s",
				dh_params_file,
				gnutls_strerror_name(r), gnutls_strerror(r));
			goto error_free_ctx;
		}
	} else {
		if (GNUTLS_E_SUCCESS > (r = load_dh_params_4096(&ctx->dh_params))) {
			ERROR(srv, "couldn't load dh parameters(%s): %s",
				gnutls_strerror_name(r), gnutls_strerror(r));
			goto error_free_ctx;
		}
	}

	gnutls_certificate_set_dh_params(ctx->server_cert, ctx->dh_params);
#ifdef USE_SNI
	if (NULL != ctx->sni_fallback_cert) {
		gnutls_certificate_set_dh_params(ctx->sni_fallback_cert, ctx->dh_params);
	}
#endif

	if (priority) {
		const char *errpos = NULL;
		gnutls_priority_t prio;
		GString *s = srv->main_worker->tmp_str;

		if (GNUTLS_E_SUCCESS > (r = gnutls_priority_init(&prio, priority, &errpos))) {
			ERROR(srv, "gnutls_priority_init failed(priority '%s', error at '%s') (%s): %s",
				priority, errpos,
				gnutls_strerror_name(r), gnutls_strerror(r));
			goto error_free_ctx;
		}

		gnutls_priority_deinit(ctx->server_priority);
		ctx->server_priority = prio;

		if (protect_against_beast) {
			g_string_assign(s, priority);
			li_g_string_append_len(s, CONST_STR_LEN(":-CIPHER-ALL:+ARCFOUR-128"));
			if (GNUTLS_E_SUCCESS > (r = gnutls_priority_init(&prio, s->str, &errpos))) {
				ERROR(srv, "gnutls_priority_init failed(priority '%s', error at '%s') (%s): %s",
					s->str, errpos,
					gnutls_strerror_name(r), gnutls_strerror(r));
				goto error_free_ctx;
			}

			gnutls_priority_deinit(ctx->server_priority_beast);
			ctx->server_priority_beast = prio;
		}
	}

	LI_VALUE_FOREACH(entry, val)
		liValue *entryKey = li_value_list_at(entry, 0);
		liValue *entryValue = li_value_list_at(entry, 1);
		GString *entryKeyStr;

		if (LI_VALUE_STRING != li_value_type(entryKey)) continue;
		entryKeyStr = entryKey->data.string; /* keys are either NONE or STRING */

		if (g_str_equal(entryKeyStr->str, "listen")) {
			mod_gnutls_context_acquire(ctx);
			li_angel_listen(srv, entryValue->data.string, gnutls_setup_listen_cb, ctx);
		}
	LI_VALUE_END_FOREACH()

	mod_gnutls_context_release(ctx);

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

# --sec-param=high in certtool 3.0.22
certtool --get-dh-params --bits=4096 | openssl dhparam -C

-----BEGIN DH PARAMETERS-----
MIICCAKCAgEA///////////JD9qiIWjCNMTGYouA3BzRKQJOCIpnzHQCC76mOxOb
IlFKCHmONATd75UZs806QxswKwpt8l8UN0/hNW1tUcJF5IW1dmJefsb0TELppjft
awv/XLb0Brft7jhr+1qJn6WunyQRfEsf5kkoZlHs5Fs9wgB8uKFjvwWY2kg2HFXT
mmkWP6j9JM9fg2VdI9yjrZYcYvNWIIVSu57VKQdwlpZtZww1Tkq8mATxdGwIyhgh
fDKQXkYuNs474553LBgOhgObJ4Oi7Aeij7XFXfBvTFLJ3ivL9pVYFxg5lUl86pVq
5RXSJhiY+gUQFXKOWoqqxC2tMxcNBFB6M6hVIavfHLpk7PuFBFjb7wqK6nFXXQYM
fbOXD4Wm4eTHq/WujNsJM9cejJTgSiVhnc7j0iYa0u5r8S/6BtmKCGTYdgJzPshq
ZFIfKxgXeyAMu+EXV3phXWx3CYjAutlG4gjiT6B05asxQ9tb/OD9EI5LgtEgqSEI
ARpyPBKnh+bXiHGaEL26WyaZwycYavTiPBqUaDS2FQvaJYPpyirUTOjbu8LbBN6O
+S6O/BQfvsqmKHxZR05rwF2ZspZPoJDDoiM7oYZRW+ftH2EpcM7i16+4G912IXBI
HNAGkSfVsFqpk7TqmI2P3cGG/7fckKbAj030Nck0BjGZ//////////8CAQU=
-----END DH PARAMETERS-----

*/

static int load_dh_params_4096(gnutls_dh_params_t *dh_params) {
	static const unsigned char dh4096_p[]={
		0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xC9,0x0F,0xDA,0xA2,
		0x21,0x68,0xC2,0x34,0xC4,0xC6,0x62,0x8B,0x80,0xDC,0x1C,0xD1,
		0x29,0x02,0x4E,0x08,0x8A,0x67,0xCC,0x74,0x02,0x0B,0xBE,0xA6,
		0x3B,0x13,0x9B,0x22,0x51,0x4A,0x08,0x79,0x8E,0x34,0x04,0xDD,
		0xEF,0x95,0x19,0xB3,0xCD,0x3A,0x43,0x1B,0x30,0x2B,0x0A,0x6D,
		0xF2,0x5F,0x14,0x37,0x4F,0xE1,0x35,0x6D,0x6D,0x51,0xC2,0x45,
		0xE4,0x85,0xB5,0x76,0x62,0x5E,0x7E,0xC6,0xF4,0x4C,0x42,0xE9,
		0xA6,0x37,0xED,0x6B,0x0B,0xFF,0x5C,0xB6,0xF4,0x06,0xB7,0xED,
		0xEE,0x38,0x6B,0xFB,0x5A,0x89,0x9F,0xA5,0xAE,0x9F,0x24,0x11,
		0x7C,0x4B,0x1F,0xE6,0x49,0x28,0x66,0x51,0xEC,0xE4,0x5B,0x3D,
		0xC2,0x00,0x7C,0xB8,0xA1,0x63,0xBF,0x05,0x98,0xDA,0x48,0x36,
		0x1C,0x55,0xD3,0x9A,0x69,0x16,0x3F,0xA8,0xFD,0x24,0xCF,0x5F,
		0x83,0x65,0x5D,0x23,0xDC,0xA3,0xAD,0x96,0x1C,0x62,0xF3,0x56,
		0x20,0x85,0x52,0xBB,0x9E,0xD5,0x29,0x07,0x70,0x96,0x96,0x6D,
		0x67,0x0C,0x35,0x4E,0x4A,0xBC,0x98,0x04,0xF1,0x74,0x6C,0x08,
		0xCA,0x18,0x21,0x7C,0x32,0x90,0x5E,0x46,0x2E,0x36,0xCE,0x3B,
		0xE3,0x9E,0x77,0x2C,0x18,0x0E,0x86,0x03,0x9B,0x27,0x83,0xA2,
		0xEC,0x07,0xA2,0x8F,0xB5,0xC5,0x5D,0xF0,0x6F,0x4C,0x52,0xC9,
		0xDE,0x2B,0xCB,0xF6,0x95,0x58,0x17,0x18,0x39,0x95,0x49,0x7C,
		0xEA,0x95,0x6A,0xE5,0x15,0xD2,0x26,0x18,0x98,0xFA,0x05,0x10,
		0x15,0x72,0x8E,0x5A,0x8A,0xAA,0xC4,0x2D,0xAD,0x33,0x17,0x0D,
		0x04,0x50,0x7A,0x33,0xA8,0x55,0x21,0xAB,0xDF,0x1C,0xBA,0x64,
		0xEC,0xFB,0x85,0x04,0x58,0xDB,0xEF,0x0A,0x8A,0xEA,0x71,0x57,
		0x5D,0x06,0x0C,0x7D,0xB3,0x97,0x0F,0x85,0xA6,0xE1,0xE4,0xC7,
		0xAB,0xF5,0xAE,0x8C,0xDB,0x09,0x33,0xD7,0x1E,0x8C,0x94,0xE0,
		0x4A,0x25,0x61,0x9D,0xCE,0xE3,0xD2,0x26,0x1A,0xD2,0xEE,0x6B,
		0xF1,0x2F,0xFA,0x06,0xD9,0x8A,0x08,0x64,0xD8,0x76,0x02,0x73,
		0x3E,0xC8,0x6A,0x64,0x52,0x1F,0x2B,0x18,0x17,0x7B,0x20,0x0C,
		0xBB,0xE1,0x17,0x57,0x7A,0x61,0x5D,0x6C,0x77,0x09,0x88,0xC0,
		0xBA,0xD9,0x46,0xE2,0x08,0xE2,0x4F,0xA0,0x74,0xE5,0xAB,0x31,
		0x43,0xDB,0x5B,0xFC,0xE0,0xFD,0x10,0x8E,0x4B,0x82,0xD1,0x20,
		0xA9,0x21,0x08,0x01,0x1A,0x72,0x3C,0x12,0xA7,0x87,0xE6,0xD7,
		0x88,0x71,0x9A,0x10,0xBD,0xBA,0x5B,0x26,0x99,0xC3,0x27,0x18,
		0x6A,0xF4,0xE2,0x3C,0x1A,0x94,0x68,0x34,0xB6,0x15,0x0B,0xDA,
		0x25,0x83,0xE9,0xCA,0x2A,0xD4,0x4C,0xE8,0xDB,0xBB,0xC2,0xDB,
		0x04,0xDE,0x8E,0xF9,0x2E,0x8E,0xFC,0x14,0x1F,0xBE,0xCA,0xA6,
		0x28,0x7C,0x59,0x47,0x4E,0x6B,0xC0,0x5D,0x99,0xB2,0x96,0x4F,
		0xA0,0x90,0xC3,0xA2,0x23,0x3B,0xA1,0x86,0x51,0x5B,0xE7,0xED,
		0x1F,0x61,0x29,0x70,0xCE,0xE2,0xD7,0xAF,0xB8,0x1B,0xDD,0x76,
		0x21,0x70,0x48,0x1C,0xD0,0x06,0x91,0x27,0xD5,0xB0,0x5A,0xA9,
		0x93,0xB4,0xEA,0x98,0x8D,0x8F,0xDD,0xC1,0x86,0xFF,0xB7,0xDC,
		0x90,0xA6,0xC0,0x8F,0x4D,0xF4,0x35,0xC9,0x34,0x06,0x31,0x99,
		0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
		};
	static const unsigned char dh4096_g[]={
		0x05,
		};
	static const gnutls_datum_t prime = { (unsigned char*) dh4096_p, sizeof(dh4096_p) };
	static const gnutls_datum_t generator = { (unsigned char*) dh4096_g, sizeof(dh4096_g) };
	int r;
	gnutls_dh_params_t params;

	if (GNUTLS_E_SUCCESS > (r = gnutls_dh_params_init(&params))) return r;
	if (GNUTLS_E_SUCCESS > (r = gnutls_dh_params_import_raw(params, &prime, &generator))) {
		gnutls_dh_params_deinit(params);
		return r;
	}
	*dh_params = params;
	return GNUTLS_E_SUCCESS;
}

/*
 * mod_openssl - ssl support
 *
 * Author:
 *     Copyright (c) 2009-2013 Stefan Bühler, Joe Presbrey
 */

#include <lighttpd/base.h>
#include <lighttpd/throttle.h>
#include <lighttpd/plugin_core.h>

#include "openssl_filter.h"

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/rand.h>

# ifndef OPENSSL_NO_DH
#  include <openssl/dh.h>
#  define USE_OPENSSL_DH
static DH* load_dh_params_4096(void);
# endif

#if OPENSSL_VERSION_NUMBER >= 0x0090800fL
# ifndef OPENSSL_NO_ECDH
#  include <openssl/ecdh.h>
#  define USE_OPENSSL_ECDH
# endif
#endif


LI_API gboolean mod_openssl_init(liModules *mods, liModule *mod);
LI_API gboolean mod_openssl_free(liModules *mods, liModule *mod);


typedef struct openssl_connection_ctx openssl_connection_ctx;
typedef struct openssl_context openssl_context;

struct openssl_connection_ctx {
	liConnection *con;
	liOpenSSLFilter *ssl_filter;

	liIOStream *sock_stream;
	gpointer simple_socket_data;
};

struct openssl_context {
	gint refcount;

	SSL_CTX *ssl_ctx;
};

enum {
	SE_CLIENT      = 0x1,
	SE_CLIENT_CERT = 0x2,
	SE_SERVER      = 0x4,
	SE_SERVER_CERT = 0x8
};

static openssl_context* mod_openssl_context_new(void) {
	openssl_context *ctx = g_slice_new0(openssl_context);
	ctx->refcount = 1;
	return ctx;
}

static void mod_openssl_context_release(openssl_context *ctx) {
	if (NULL == ctx) return;
	LI_FORCE_ASSERT(g_atomic_int_get(&ctx->refcount) > 0);
	if (g_atomic_int_dec_and_test(&ctx->refcount)) {
		if (NULL != ctx->ssl_ctx) {
			SSL_CTX_free(ctx->ssl_ctx);
			ctx->ssl_ctx = NULL;
		}

		g_slice_free(openssl_context, ctx);
	}
}

static void mod_openssl_context_acquire(openssl_context *ctx) {
	LI_FORCE_ASSERT(g_atomic_int_get(&ctx->refcount) > 0);
	g_atomic_int_inc(&ctx->refcount);
}



static void tcp_io_cb(liIOStream *stream, liIOStreamEvent event) {
	openssl_connection_ctx *conctx = stream->data;
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
		LI_FORCE_ASSERT(NULL == conctx->ssl_filter);
		LI_FORCE_ASSERT(NULL == conctx->con);
		stream->data = NULL;
		g_slice_free(openssl_connection_ctx, conctx);
		return;
	default:
		break;
	}
}

static void handshake_cb(liOpenSSLFilter *f, gpointer data, liStream *plain_source, liStream *plain_drain) {
	openssl_connection_ctx *conctx = data;
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

static void close_cb(liOpenSSLFilter *f, gpointer data) {
	openssl_connection_ctx *conctx = data;
	liConnection *con = conctx->con;
	LI_FORCE_ASSERT(conctx->ssl_filter == f);

	conctx->ssl_filter = NULL;
	li_openssl_filter_free(f);

	if (NULL != conctx->con) {
		liStream *raw_out = con->con_sock.raw_out, *raw_in = con->con_sock.raw_in;
		LI_FORCE_ASSERT(con->con_sock.data == conctx);
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

static const liOpenSSLFilterCallbacks filter_callbacks = {
	handshake_cb,
	close_cb
};

static void openssl_tcp_finished(liConnection *con, gboolean aborted) {
	openssl_connection_ctx *conctx = con->con_sock.data;
	UNUSED(aborted);

	con->info.is_ssl = FALSE;
	con->con_sock.callbacks = NULL;

	if (NULL != conctx) {
		LI_FORCE_ASSERT(con == conctx->con);
		close_cb(conctx->ssl_filter, conctx);
	}

	{
		liStream *raw_out = con->con_sock.raw_out, *raw_in = con->con_sock.raw_in;
		con->con_sock.raw_out = con->con_sock.raw_in = NULL;
		if (NULL != raw_out) { li_stream_reset(raw_out); li_stream_release(raw_out); }
		if (NULL != raw_in) { li_stream_reset(raw_in); li_stream_release(raw_in); }
	}
}

static liThrottleState* openssl_tcp_throttle_out(liConnection *con) {
	openssl_connection_ctx *conctx = con->con_sock.data;
	if (NULL == conctx) return NULL;
	if (NULL == conctx->sock_stream->throttle_out) conctx->sock_stream->throttle_out = li_throttle_new();
	return conctx->sock_stream->throttle_out;
}

static liThrottleState* openssl_tcp_throttle_in(liConnection *con) {
	openssl_connection_ctx *conctx = con->con_sock.data;
	if (NULL == conctx) return NULL;
	if (NULL == conctx->sock_stream->throttle_in) conctx->sock_stream->throttle_in = li_throttle_new();
	return conctx->sock_stream->throttle_in;
}

static const liConnectionSocketCallbacks openssl_tcp_cbs = {
	openssl_tcp_finished,
	openssl_tcp_throttle_out,
	openssl_tcp_throttle_in
};

static gboolean openssl_con_new(liConnection *con, int fd) {
	liEventLoop *loop = &con->wrk->loop;
	liServer *srv = con->srv;
	openssl_context *ctx = con->srv_sock->data;
	openssl_connection_ctx *conctx = g_slice_new0(openssl_connection_ctx);

	conctx->sock_stream = li_iostream_new(con->wrk, fd, tcp_io_cb, conctx);

	conctx->ssl_filter = li_openssl_filter_new(srv, con->wrk, &filter_callbacks, conctx, ctx->ssl_ctx,
		&conctx->sock_stream->stream_in, &conctx->sock_stream->stream_out);

	if (NULL == conctx->ssl_filter) {
		ERROR(srv, "SSL_new: %s", ERR_error_string(ERR_get_error(), NULL));
		fd = li_iostream_reset(conctx->sock_stream);
		close(fd);
		g_slice_free(openssl_connection_ctx, conctx);
		return FALSE;
	}

	conctx->con = con;
	con->con_sock.data = conctx;
	con->con_sock.callbacks = &openssl_tcp_cbs;
	con->con_sock.raw_out = li_stream_plug_new(loop);
	con->con_sock.raw_in = li_stream_plug_new(loop);
	con->info.is_ssl = TRUE;

	return TRUE;
}


static void openssl_sock_release(liServerSocket *srv_sock) {
	openssl_context *ctx = srv_sock->data;

	mod_openssl_context_release(ctx);
}

static void openssl_setenv_X509_add_entries(liVRequest *vr, X509 *x509, const gchar *prefix, guint prefix_len) {
	guint i, j;
	GString *k = vr->wrk->tmp_str;

	X509_NAME *xn = X509_get_subject_name(x509);
	X509_NAME_ENTRY *xe;
	const char * xobjsn;

	g_string_truncate(k, 0);
	g_string_append_len(k, prefix, prefix_len);

	for (i = 0, j = X509_NAME_entry_count(xn); i < j; ++i) {
		if (!(xe = X509_NAME_get_entry(xn, i))
			|| !(xobjsn = OBJ_nid2sn(OBJ_obj2nid((ASN1_OBJECT*)X509_NAME_ENTRY_get_object(xe)))))
			continue;
		g_string_truncate(k, prefix_len);
		g_string_append(k, xobjsn);
		li_environment_set(&vr->env, GSTR_LEN(k), (const gchar *)xe->value->data, xe->value->length);
	}
}

static void openssl_setenv_X509_add_PEM(liVRequest *vr, X509 *x509, const gchar *key, guint key_len) {
	gint n;
	GString *v = vr->wrk->tmp_str;

	BIO *bio;
	if (NULL != (bio = BIO_new(BIO_s_mem()))) {
		PEM_write_bio_X509(bio, x509);
		n = BIO_pending(bio);
		g_string_set_size(v, n);
		BIO_read(bio, v->str, n);
		BIO_free(bio);
		li_environment_set(&vr->env, key, key_len, GSTR_LEN(v));
	}
}

static liHandlerResult openssl_setenv(liVRequest *vr, gpointer param, gpointer *context) {
	liConnection *con;
	openssl_connection_ctx *conctx;
	SSL *ssl;
	X509 *x0=NULL, *x1=NULL;
	guint params = GPOINTER_TO_UINT(param);

	UNUSED(context);

	if (!(con = li_connection_from_vrequest(vr))
		|| !(con->srv_sock && con->srv_sock->new_cb == openssl_con_new)
		|| !(conctx = con->con_sock.data)
		|| !(ssl = li_openssl_filter_ssl(conctx->ssl_filter)))
		return LI_HANDLER_GO_ON;

	if ((params & SE_CLIENT) && (x1 || (x1 = SSL_get_peer_certificate(ssl))))
		openssl_setenv_X509_add_entries(vr, x1, CONST_STR_LEN("SSL_CLIENT_S_DN_"));
	if ((params & SE_CLIENT_CERT) && (x1 || (x1 = SSL_get_peer_certificate(ssl))))
		openssl_setenv_X509_add_PEM(vr, x1, CONST_STR_LEN("SSL_CLIENT_CERT"));
	if ((params & SE_SERVER) && (x0 || (x0 = SSL_get_certificate(ssl))))
		openssl_setenv_X509_add_entries(vr, x0, CONST_STR_LEN("SSL_SERVER_S_DN_"));
	if ((params & SE_SERVER_CERT) && (x0 || (x0 = SSL_get_certificate(ssl))))
		openssl_setenv_X509_add_PEM(vr, x0, CONST_STR_LEN("SSL_SERVER_CERT"));

	/* only peer increases ref count */
	if (x1) X509_free(x1);

	return LI_HANDLER_GO_ON;
}

static const char openssl_setenv_config_error[] = "openssl.setenv expects a string or a list of strings consisting of: client, client-cert, server, server-cert";
static liAction* openssl_setenv_create(liServer *srv, liWorker *wrk, liPlugin* p, liValue *val, gpointer userdata) {
	guint params = 0;
	UNUSED(srv); UNUSED(wrk); UNUSED(p); UNUSED(userdata);

	val = li_value_get_single_argument(val);

	if (LI_VALUE_STRING == li_value_type(val)) {
		li_value_wrap_in_list(val);
	}

	if (LI_VALUE_LIST != li_value_type(val)) {
		ERROR(srv, "%s", openssl_setenv_config_error);
		return NULL;
	}

	LI_VALUE_FOREACH(v, val)
		if (LI_VALUE_STRING != li_value_type(v)) {
			ERROR(srv, "%s", openssl_setenv_config_error);
			return NULL;
		}
		if (li_strncase_equal(v->data.string, CONST_STR_LEN("client"))) {
			params |= SE_CLIENT;
		} else if (li_strncase_equal(v->data.string, CONST_STR_LEN("client-cert"))) {
			params |= SE_CLIENT_CERT;
		} else if (li_strncase_equal(v->data.string, CONST_STR_LEN("server"))) {
			params |= SE_SERVER;
		} else if (li_strncase_equal(v->data.string, CONST_STR_LEN("server-cert"))) {
			params |= SE_SERVER_CERT;
		} else {
			ERROR(srv, "%s", openssl_setenv_config_error);
			return NULL;
		}
	LI_VALUE_END_FOREACH()

	return li_action_new_function(openssl_setenv, NULL, NULL, GUINT_TO_POINTER(params));
}

static void openssl_setup_listen_cb(liServer *srv, int fd, gpointer data) {
	openssl_context *ctx = data;
	liServerSocket *srv_sock;
	UNUSED(data);

	if (-1 == fd) {
		mod_openssl_context_release(ctx);
		return;
	}

	srv_sock = li_server_listen(srv, fd);

	srv_sock->data = ctx;

	srv_sock->new_cb = openssl_con_new;
	srv_sock->release_cb = openssl_sock_release;
}

static gboolean openssl_options_set_string(long *options, GString *s) {
#define S(s) CONST_STR_LEN(s)
	static const struct {
		char *name;    /* without "NO_" prefix */
		guint name_len;
		long value;
		char positive; /* 0 means option is usually prefixed with "NO_"; otherwise use 1 */
	} option_table[] = {
		{S("MICROSOFT_SESS_ID_BUG"), SSL_OP_MICROSOFT_SESS_ID_BUG, 1},
		{S("NETSCAPE_CHALLENGE_BUG"), SSL_OP_NETSCAPE_CHALLENGE_BUG, 1},
#ifdef SSL_OP_LEGACY_SERVER_CONNECT
		{S("LEGACY_SERVER_CONNECT"), SSL_OP_LEGACY_SERVER_CONNECT, 1},
#endif
		{S("NETSCAPE_REUSE_CIPHER_CHANGE_BUG"), SSL_OP_NETSCAPE_REUSE_CIPHER_CHANGE_BUG, 1},
		{S("SSLREF2_REUSE_CERT_TYPE_BUG"), SSL_OP_SSLREF2_REUSE_CERT_TYPE_BUG, 1},
		{S("MICROSOFT_BIG_SSLV3_BUFFER"), SSL_OP_MICROSOFT_BIG_SSLV3_BUFFER, 1},
		{S("MSIE_SSLV2_RSA_PADDING"), SSL_OP_MSIE_SSLV2_RSA_PADDING, 1},
		{S("SSLEAY_080_CLIENT_DH_BUG"), SSL_OP_SSLEAY_080_CLIENT_DH_BUG, 1},
		{S("TLS_D5_BUG"), SSL_OP_TLS_D5_BUG, 1},
		{S("TLS_BLOCK_PADDING_BUG"), SSL_OP_TLS_BLOCK_PADDING_BUG, 1},
#ifdef SSL_OP_DONT_INSERT_EMPTY_FRAGMENTS
		{S("DONT_INSERT_EMPTY_FRAGMENTS"), SSL_OP_DONT_INSERT_EMPTY_FRAGMENTS, 1},
#endif
		{S("ALL"), SSL_OP_ALL, 1},
#ifdef SSL_OP_NO_QUERY_MTU
		{S("QUERY_MTU"), SSL_OP_NO_QUERY_MTU, 0},
#endif
#ifdef SSL_OP_COOKIE_EXCHANGE
		{S("COOKIE_EXCHANGE"), SSL_OP_COOKIE_EXCHANGE, 1},
#endif
#ifdef SSL_OP_NO_TICKET
		{S("TICKET"), SSL_OP_NO_TICKET, 0},
#endif
#ifdef SSL_OP_CISCO_ANYCONNECT
		{S("CISCO_ANYCONNECT"), SSL_OP_CISCO_ANYCONNECT, 1},
#endif
#ifdef SSL_OP_NO_SESSION_RESUMPTION_ON_RENEGOTIATION
		{S("SESSION_RESUMPTION_ON_RENEGOTIATION"), SSL_OP_NO_SESSION_RESUMPTION_ON_RENEGOTIATION, 0},
#endif
#ifdef SSL_OP_NO_COMPRESSION
		{S("COMPRESSION"), SSL_OP_NO_COMPRESSION, 0},
#endif
#ifdef SSL_OP_ALLOW_UNSAFE_LEGACY_RENEGOTIATION
		{S("ALLOW_UNSAFE_LEGACY_RENEGOTIATION"), SSL_OP_ALLOW_UNSAFE_LEGACY_RENEGOTIATION, 1},
#endif
#ifdef SSL_OP_SINGLE_ECDH_USE
		{S("SINGLE_ECDH_USE"), SSL_OP_SINGLE_ECDH_USE, 1},
#endif
		{S("SINGLE_DH_USE"), SSL_OP_SINGLE_DH_USE, 1},
		{S("EPHEMERAL_RSA"), SSL_OP_EPHEMERAL_RSA, 1},
#ifdef SSL_OP_CIPHER_SERVER_PREFERENCE
		{S("CIPHER_SERVER_PREFERENCE"), SSL_OP_CIPHER_SERVER_PREFERENCE, 1},
#endif
		{S("TLS_ROLLBACK_BUG"), SSL_OP_TLS_ROLLBACK_BUG, 1},
		{S("SSLv2"), SSL_OP_NO_SSLv2, 0},
		{S("SSLv3"), SSL_OP_NO_SSLv3, 0},
		{S("TLSv1"), SSL_OP_NO_TLSv1, 0},
		{S("PKCS1_CHECK_1"), SSL_OP_PKCS1_CHECK_1, 1},
		{S("PKCS1_CHECK_2"), SSL_OP_PKCS1_CHECK_2, 1},
		{S("NETSCAPE_CA_DN_BUG"), SSL_OP_NETSCAPE_CA_DN_BUG, 1},
		{S("NETSCAPE_DEMO_CIPHER_CHANGE_BUG"), SSL_OP_NETSCAPE_DEMO_CIPHER_CHANGE_BUG, 1},
#ifdef SSL_OP_CRYPTOPRO_TLSEXT_BUG
		{S("CRYPTOPRO_TLSEXT_BUG"), SSL_OP_CRYPTOPRO_TLSEXT_BUG, 1}
#endif
	};
#undef S

	GString key = *s;
	guint i;
	char positive = 1;

	if (0 == g_ascii_strncasecmp(key.str, CONST_STR_LEN("NO_"))) {
		key.str += 3;
		key.len -= 3;
		positive = 0;
	}

	for (i = 0; i < G_N_ELEMENTS(option_table); i++) {
		if (option_table[i].name_len == key.len && 0 == g_ascii_strcasecmp(key.str, option_table[i].name)) {
			if (option_table[i].positive == positive) {
				*options |= option_table[i].value;
			} else {
				*options &= ~option_table[i].value;
			}
			return TRUE;
		}
	}
	return FALSE;
}

static int openssl_verify_any_cb(int ok, X509_STORE_CTX *ctx) { UNUSED(ok); UNUSED(ctx); return 1; }

static gboolean openssl_setup(liServer *srv, liPlugin* p, liValue *val, gpointer userdata) {
	openssl_context *ctx;
	STACK_OF(X509_NAME) *client_ca_list;

	const char
		*default_ciphers = "ECDHE-RSA-AES256-SHA384:AES256-SHA256:RC4-SHA:RC4:HIGH:!MD5:!aNULL:!EDH:!AESGCM",
		*default_ecdh_curve = "prime256v1";

	/* setup defaults */
	gboolean
		have_listen_parameter = FALSE,
		have_options_parameter = FALSE,
		have_verify_parameter = FALSE,
		have_verify_depth_parameter = FALSE,
		have_verify_any_parameter = FALSE,
		have_verify_require_parameter = FALSE;
	const char
		*ciphers = NULL, *pemfile = NULL, *ca_file = NULL, *client_ca_file = NULL, *dh_params_file = NULL, *ecdh_curve = NULL;
	long
		options = SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3 | SSL_OP_CIPHER_SERVER_PREFERENCE | SSL_OP_SINGLE_DH_USE
#ifdef SSL_OP_NO_COMPRESSION
			| SSL_OP_NO_COMPRESSION
#endif
#ifdef USE_OPENSSL_ECDH
			| SSL_OP_SINGLE_ECDH_USE
#endif
		;
	guint
		verify_mode = 0, verify_depth = 1;
	gboolean
		verify_any = FALSE;

	UNUSED(p); UNUSED(userdata);

	val = li_value_get_single_argument(val);

	if (NULL == (val = li_value_to_key_value_list(val))) {
		ERROR(srv, "%s", "openssl expects a hash/key-value list as parameter");
		return FALSE;
	}

	LI_VALUE_FOREACH(entry, val)
		liValue *entryKey = li_value_list_at(entry, 0);
		liValue *entryValue = li_value_list_at(entry, 1);
		GString *entryKeyStr;

		if (LI_VALUE_STRING != li_value_type(entryKey)) {
			ERROR(srv, "%s", "openssl doesn't take default keys");
			return FALSE;
		}
		entryKeyStr = entryKey->data.string; /* keys are either NONE or STRING */

		if (g_str_equal(entryKeyStr->str, "listen")) {
			if (LI_VALUE_STRING != li_value_type(entryValue)) {
				ERROR(srv, "%s", "openssl listen expects a string as parameter");
				return FALSE;
			}
			have_listen_parameter = TRUE;
		} else if (g_str_equal(entryKeyStr->str, "pemfile")) {
			if (LI_VALUE_STRING != li_value_type(entryValue)) {
				ERROR(srv, "%s", "openssl pemfile expects a string as parameter");
				return FALSE;
			}
			if (NULL != pemfile) {
				ERROR(srv, "openssl unexpected duplicate parameter %s", entryKeyStr->str);
				return FALSE;
			}
			pemfile = entryValue->data.string->str;
		} else if (g_str_equal(entryKeyStr->str, "ca-file")) {
			if (LI_VALUE_STRING != li_value_type(entryValue)) {
				ERROR(srv, "%s", "openssl ca-file expects a string as parameter");
				return FALSE;
			}
			if (NULL != ca_file) {
				ERROR(srv, "openssl unexpected duplicate parameter %s", entryKeyStr->str);
				return FALSE;
			}
			ca_file = entryValue->data.string->str;
		} else if (g_str_equal(entryKeyStr->str, "ciphers")) {
			if (LI_VALUE_STRING != li_value_type(entryValue)) {
				ERROR(srv, "%s", "openssl ciphers expects a string as parameter");
				return FALSE;
			}
			if (NULL != ciphers) {
				ERROR(srv, "openssl unexpected duplicate parameter %s", entryKeyStr->str);
				return FALSE;
			}
			ciphers = entryValue->data.string->str;
		} else if (g_str_equal(entryKeyStr->str, "dh-params")) {
#ifndef USE_OPENSSL_DH
			WARNING(srv, "%s", "the openssl library in use doesn't support DH => dh-params has no effect");
#endif
			if (LI_VALUE_STRING != li_value_type(entryValue)) {
				ERROR(srv, "%s", "openssl dh-params expects a string as parameter");
				return FALSE;
			}
			if (NULL != dh_params_file) {
				ERROR(srv, "openssl unexpected duplicate parameter %s", entryKeyStr->str);
				return FALSE;
			}
			dh_params_file = entryValue->data.string->str;
		} else if (g_str_equal(entryKeyStr->str, "ecdh-curve")) {
#ifndef USE_OPENSSL_ECDH
			WARNING(srv, "%s", "the openssl library in use doesn't support ECDH => ecdh-curve has no effect");
#endif
			if (LI_VALUE_STRING != li_value_type(entryValue)) {
				ERROR(srv, "%s", "openssl ecdh-curve expects a string as parameter");
				return FALSE;
			}
			if (NULL != ecdh_curve) {
				ERROR(srv, "openssl unexpected duplicate parameter %s", entryKeyStr->str);
				return FALSE;
			}
			ecdh_curve = entryValue->data.string->str;
		} else if (g_str_equal(entryKeyStr->str, "options")) {
			/* accept single parameter too */
			if (LI_VALUE_STRING == li_value_type(entryValue)) li_value_wrap_in_list(entryValue);
			if (LI_VALUE_LIST != li_value_type(entryValue)) {
				ERROR(srv, "%s", "openssl options expects a list of strings as parameter");
				return FALSE;
			}
			if (have_options_parameter) {
				ERROR(srv, "openssl unexpected duplicate parameter %s", entryKeyStr->str);
				return FALSE;
			}
			have_options_parameter = TRUE;
			LI_VALUE_FOREACH(v, entryValue)
				if (LI_VALUE_STRING != li_value_type(v)) {
					ERROR(srv, "%s", "openssl options expects a list of strings as parameter");
					return FALSE;
				}
				if (!openssl_options_set_string(&options, v->data.string)) {
					ERROR(srv, "openssl option unknown: %s", v->data.string->str);
					return FALSE;
				}
			LI_VALUE_END_FOREACH()
		} else if (g_str_equal(entryKeyStr->str, "verify")) {
			if (LI_VALUE_BOOLEAN != li_value_type(entryValue)) {
				ERROR(srv, "%s", "openssl verify expects a boolean as parameter");
				return FALSE;
			}
			if (have_verify_parameter) {
				ERROR(srv, "openssl unexpected duplicate parameter %s", entryKeyStr->str);
				return FALSE;
			}
			have_verify_parameter = TRUE;
			if (entryValue->data.boolean)
				verify_mode |= SSL_VERIFY_PEER;
		} else if (g_str_equal(entryKeyStr->str, "verify-any")) {
			if (LI_VALUE_BOOLEAN != li_value_type(entryValue)) {
				ERROR(srv, "%s", "openssl verify-any expects a boolean as parameter");
				return FALSE;
			}
			if (have_verify_any_parameter) {
				ERROR(srv, "openssl unexpected duplicate parameter %s", entryKeyStr->str);
				return FALSE;
			}
			have_verify_any_parameter = TRUE;
			verify_any = entryValue->data.boolean;
		} else if (g_str_equal(entryKeyStr->str, "verify-depth")) {
			if (LI_VALUE_NUMBER != li_value_type(entryValue)) {
				ERROR(srv, "%s", "openssl verify-depth expects a number as parameter");
				return FALSE;
			}
			if (have_verify_depth_parameter) {
				ERROR(srv, "openssl unexpected duplicate parameter %s", entryKeyStr->str);
				return FALSE;
			}
			have_verify_depth_parameter = TRUE;
			verify_depth = entryValue->data.number;
		} else if (g_str_equal(entryKeyStr->str, "verify-require")) {
			if (LI_VALUE_BOOLEAN != li_value_type(entryValue)) {
				ERROR(srv, "%s", "openssl verify-require expects a boolean as parameter");
				return FALSE;
			}
			if (have_verify_require_parameter) {
				ERROR(srv, "openssl unexpected duplicate parameter %s", entryKeyStr->str);
				return FALSE;
			}
			have_verify_require_parameter = TRUE;
			if (entryValue->data.boolean)
				verify_mode |= SSL_VERIFY_FAIL_IF_NO_PEER_CERT;
		} else if (g_str_equal(entryKeyStr->str, "client-ca-file")) {
			if (LI_VALUE_STRING != li_value_type(entryValue)) {
				ERROR(srv, "%s", "openssl client-ca-file expects a string as parameter");
				return FALSE;
			}
			if (NULL != client_ca_file) {
				ERROR(srv, "openssl unexpected duplicate parameter %s", entryKeyStr->str);
				return FALSE;
			}
			client_ca_file = entryValue->data.string->str;
		} else {
			ERROR(srv, "invalid parameter for openssl: %s", entryKeyStr->str);
			return FALSE;

		}
	LI_VALUE_END_FOREACH()

	if (!have_listen_parameter) {
		ERROR(srv, "%s", "openssl needs a listen parameter");
		return FALSE;
	}

	if (NULL == pemfile) {
		ERROR(srv, "%s", "openssl needs a pemfile");
		return FALSE;
	}

	ctx = mod_openssl_context_new();

	if (NULL == (ctx->ssl_ctx = SSL_CTX_new(SSLv23_server_method()))) {
		ERROR(srv, "SSL_CTX_new: %s", ERR_error_string(ERR_get_error(), NULL));
		goto error_free_socket;
	}

	if (!SSL_CTX_set_options(ctx->ssl_ctx, options)) {
		ERROR(srv, "SSL_CTX_set_options(%lx): %s", options, ERR_error_string(ERR_get_error(), NULL));
		goto error_free_socket;
	}

	if (NULL == ciphers) ciphers = default_ciphers;
	if (SSL_CTX_set_cipher_list(ctx->ssl_ctx, ciphers) != 1) {
		ERROR(srv, "SSL_CTX_set_cipher_list('%s'): %s", ciphers, ERR_error_string(ERR_get_error(), NULL));
		goto error_free_socket;
	}

#ifdef USE_OPENSSL_DH
	{
		DH *dh;
		BIO *bio;

		/* Support for Diffie-Hellman key exchange */
		if (NULL != dh_params_file) {
			/* DH parameters from file */
			bio = BIO_new_file(dh_params_file, "r");
			if (bio == NULL) {
				ERROR(srv,"SSL: BIO_new_file('%s'): unable to open file", dh_params_file);
				goto error_free_socket;
			}
			dh = PEM_read_bio_DHparams(bio, NULL, NULL, NULL);
			BIO_free(bio);
			if (NULL == dh) {
				ERROR(srv, "SSL: PEM_read_bio_DHparams failed (for file '%s')", dh_params_file);
				goto error_free_socket;
			}
		} else {
			dh = load_dh_params_4096();
			if (NULL == dh) {
				ERROR(srv, "%s", "SSL: loading default DH parameters failed");
				goto error_free_socket;
			}
		}
		SSL_CTX_set_tmp_dh(ctx->ssl_ctx, dh);
		DH_free(dh);
	}
#endif

#ifdef USE_OPENSSL_ECDH
	{
		EC_KEY *ecdh;
		int ecdh_nid;

		if (NULL == ecdh_curve) ecdh_curve = default_ecdh_curve;
		ecdh_nid = OBJ_sn2nid(ecdh_curve);
		if (NID_undef == ecdh_nid) {
			ERROR(srv, "SSL: Unknown curve name '%s'", ecdh_curve);
			goto error_free_socket;
		}

		ecdh = EC_KEY_new_by_curve_name(ecdh_nid);
		if (NULL == ecdh) {
			ERROR(srv, "SSL: Unable to create curve '%s'", ecdh_curve);
			goto error_free_socket;
		}
		SSL_CTX_set_tmp_ecdh(ctx->ssl_ctx, ecdh);
		EC_KEY_free(ecdh);
	}
#else
	UNUSED(default_ecdh_curve);
#endif

	if (NULL != ca_file) {
		if (1 != SSL_CTX_load_verify_locations(ctx->ssl_ctx, ca_file, NULL)) {
			ERROR(srv, "SSL_CTX_load_verify_locations('%s'): %s", ca_file, ERR_error_string(ERR_get_error(), NULL));
			goto error_free_socket;
		}
	}

	if (SSL_CTX_use_certificate_file(ctx->ssl_ctx, pemfile, SSL_FILETYPE_PEM) < 0) {
		ERROR(srv, "SSL_CTX_use_certificate_file('%s'): %s", pemfile,
			ERR_error_string(ERR_get_error(), NULL));
		goto error_free_socket;
	}

	if (SSL_CTX_use_PrivateKey_file (ctx->ssl_ctx, pemfile, SSL_FILETYPE_PEM) < 0) {
		ERROR(srv, "SSL_CTX_use_PrivateKey_file('%s'): %s", pemfile,
			ERR_error_string(ERR_get_error(), NULL));
		goto error_free_socket;
	}

	if (SSL_CTX_check_private_key(ctx->ssl_ctx) != 1) {
		ERROR(srv, "SSL: Private key '%s' does not match the certificate public key, reason: %s", pemfile,
			ERR_error_string(ERR_get_error(), NULL));
		goto error_free_socket;
	}

	if (verify_mode) {
		if (SSL_CTX_set_session_id_context(ctx->ssl_ctx, (void*) &srv, sizeof(srv)) != 1) {
			ERROR(srv, "SSL_CTX_set_session_id_context(): %s", ERR_error_string(ERR_get_error(), NULL));
			goto error_free_socket;
		}
		SSL_CTX_set_verify(ctx->ssl_ctx, verify_mode, verify_any ? openssl_verify_any_cb : NULL);
		SSL_CTX_set_verify_depth(ctx->ssl_ctx, verify_depth);
	}

	if (NULL != client_ca_file) {
		if (SSL_CTX_load_verify_locations(ctx->ssl_ctx, client_ca_file, NULL) != 1) {
			ERROR(srv, "SSL_CTX_load_verify_locations('%s'): %s", client_ca_file, ERR_error_string(ERR_get_error(), NULL));
			goto error_free_socket;
		}
		if ((client_ca_list = SSL_load_client_CA_file(client_ca_file)) == NULL) {
			ERROR(srv, "SSL_load_client_CA_file('%s'): %s", client_ca_file, ERR_error_string(ERR_get_error(), NULL));
			goto error_free_socket;
		}
		SSL_CTX_set_client_CA_list(ctx->ssl_ctx, client_ca_list);
	}

	SSL_CTX_set_default_read_ahead(ctx->ssl_ctx, 1);
	SSL_CTX_set_mode(ctx->ssl_ctx, SSL_CTX_get_mode(ctx->ssl_ctx) | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);

	LI_VALUE_FOREACH(entry, val)
		liValue *entryKey = li_value_list_at(entry, 0);
		liValue *entryValue = li_value_list_at(entry, 1);
		GString *entryKeyStr;

		if (LI_VALUE_STRING != li_value_type(entryKey)) continue;
		entryKeyStr = entryKey->data.string; /* keys are either NONE or STRING */

		if (g_str_equal(entryKeyStr->str, "listen")) {
			mod_openssl_context_acquire(ctx);
			li_angel_listen(srv, entryValue->data.string, openssl_setup_listen_cb, ctx);
		}
	LI_VALUE_END_FOREACH()

	mod_openssl_context_release(ctx);

	return TRUE;

error_free_socket:
	mod_openssl_context_release(ctx);

	return FALSE;
}

static const liPluginOption options[] = {
	{ NULL, 0, 0, NULL }
};

static const liPluginAction actions[] = {
	{ "openssl.setenv", openssl_setenv_create, NULL },

	{ NULL, NULL, NULL }
};

static const liPluginSetup setups[] = {
	{ "openssl", openssl_setup, NULL },

	{ NULL, NULL, NULL }
};


static void plugin_init(liServer *srv, liPlugin *p, gpointer userdata) {
	UNUSED(srv); UNUSED(userdata);

	p->options = options;
	p->actions = actions;
	p->setups = setups;
}

static GMutex** ssl_locks;

static void ssl_lock_cb(int mode, int n, const char *file, int line) {
	UNUSED(file);
	UNUSED(line);

	if (mode & CRYPTO_LOCK) {
		g_mutex_lock(ssl_locks[n]);
	} else if (mode & CRYPTO_UNLOCK) {
		g_mutex_unlock(ssl_locks[n]);
	}
}

static unsigned long ssl_id_cb(void) {
	return (intptr_t) g_thread_self();
}

static void sslthread_init(void) {
	int n = CRYPTO_num_locks(), i;

	ssl_locks = g_slice_alloc0(sizeof(GMutex*) * n);

	for (i = 0; i < n; i++) {
		ssl_locks[i] = g_mutex_new();
	}

	CRYPTO_set_locking_callback(ssl_lock_cb);
	CRYPTO_set_id_callback(ssl_id_cb);
}

static void sslthread_free(void) {
	int n = CRYPTO_num_locks(), i;

	for (i = 0; i < n; i++) {
		g_mutex_free(ssl_locks[i]);
	}

	g_slice_free1(sizeof(GMutex*) * n, ssl_locks);
}

gboolean mod_openssl_init(liModules *mods, liModule *mod) {
	MODULE_VERSION_CHECK(mods);

	sslthread_init();

	SSL_load_error_strings();
	SSL_library_init();

	if (0 == RAND_status()) {
		ERROR(mods->main, "SSL: %s", "not enough entropy in the pool");
		return FALSE;
	}

	mod->config = li_plugin_register(mods->main, "mod_openssl", plugin_init, NULL);

	return mod->config != NULL;
}

gboolean mod_openssl_free(liModules *mods, liModule *mod) {
	if (mod->config)
		li_plugin_free(mods->main, mod->config);

	ERR_free_strings();

	sslthread_free();

	return TRUE;
}

#ifdef USE_OPENSSL_DH
static DH* load_dh_params_4096(void) {
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

	DH *dh = DH_new();
	if (NULL == dh) return NULL;

	dh->p = BN_bin2bn(dh4096_p, sizeof(dh4096_p), NULL);
	dh->g = BN_bin2bn(dh4096_g, sizeof(dh4096_g), NULL);

	if (NULL == dh->p || NULL == dh->g) {
		DH_free(dh);
		return NULL;
	}

	return dh;
}
#endif

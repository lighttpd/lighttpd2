/*
 * mod_openssl - ssl support
 *
 * Description:
 *     mod_openssl listens on separate sockets for ssl connections (https://...)
 *
 * Setups:
 *     openssl        - setup a ssl socket; takes a hash of following parameters:
 *       listen         - (mandatory) the socket address (same as standard listen)
 *       pemfile        - (mandatory) contains key and direct certificate for the key (PEM format)
 *       ca-file        - contains certificate chain
 *       ciphers        - contains colon separated list of allowed ciphers
 *                        default: "ECDHE-RSA-AES256-SHA384:AES256-SHA256:RC4-SHA:RC4:HIGH:!MD5:!aNULL:!EDH:!AESGCM"
 *       options        - (list of strings) set OpenSSL-specific options (default: NO_SSLv2, CIPHER_SERVER_PREFERENCE, NO_COMPRESSION)
 *                        to overwrite defaults you need to explicitly specify the reverse flag (toggle "NO_" prefix)
 *                        example: use sslv2 and compression: [ options: ("SSLv2", "COMPRESSION") ]
 *       verify         - (boolean) enable client certificate verification (default: false)
 *       verify-any     - (boolean) allow all CAs and self-signed certificates (for manual checking, default: false)
 *       verify-depth   - (number) sets client verification depth (default: 1)
 *       verify-require - (boolean) abort clients failing verification (default: false)
 *       client-ca-file - (string) path to file containing client CA certificates
 *
 * Actions:
 *     openssl.setenv [options] - set SSL environment strings
 *         options: (list), may contain strings:
 *             "client"      - set SSL_CLIENT_S_DN_ short-named entries
 *             "client-cert" - set SSL_CLIENT_CERT to client certificate PEM
 *             "server"      - set SSL_SERVER_S_DN_ short-named entries
 *             "server-cert" - set SSL_SERVER_CERT to server certificate PEM
 *
 * Example config:
 *     setup openssl [ "listen": "0.0.0.0:8443", "pemfile": "server.pem" ];
 *     setup openssl [ "listen": "[::]:8443", "pemfile": "server.pem" ];
 *     openssl.setenv "client";
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
	SSL_CTX *ssl_ctx;
};

enum {
	SE_CLIENT      = 0x1,
	SE_CLIENT_CERT = 0x2,
	SE_SERVER      = 0x4,
	SE_SERVER_CERT = 0x8
};


static void tcp_io_cb(liIOStream *stream, liIOStreamEvent event) {
	openssl_connection_ctx *conctx = stream->data;
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
		assert(NULL == conctx->ssl_filter);
		assert(NULL == conctx->con);
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
	assert(conctx->ssl_filter == f);

	conctx->ssl_filter = NULL;
	li_openssl_filter_free(f);

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
		assert(con == conctx->con);
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
		li_iostream_reset(conctx->sock_stream);
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

	if (!ctx) return;

	SSL_CTX_free(ctx->ssl_ctx);
	g_slice_free(openssl_context, ctx);
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
	guint i;
	liValue *v;
	guint params = 0;

	UNUSED(srv); UNUSED(wrk); UNUSED(p); UNUSED(userdata);

	if (val && val->type == LI_VALUE_STRING)
		li_value_wrap_in_list(val);

	if (!val || val->type != LI_VALUE_LIST) {
		ERROR(srv, "%s", openssl_setenv_config_error);
		return NULL;
	}

	for (i = 0; i < val->data.list->len; i++) {
		v = g_array_index(val->data.list, liValue*, i);
		if (v->type != LI_VALUE_STRING) {
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
	}

	return li_action_new_function(openssl_setenv, NULL, NULL, GUINT_TO_POINTER(params));
}

static void openssl_setup_listen_cb(liServer *srv, int fd, gpointer data) {
	openssl_context *ctx = data;
	liServerSocket *srv_sock;
	UNUSED(data);

	if (-1 == fd) {
		SSL_CTX_free(ctx->ssl_ctx);
		g_slice_free(openssl_context, ctx);
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
	GHashTableIter hti;
	gpointer hkey, hvalue;
	GString *htkey;
	liValue *htval;
	STACK_OF(X509_NAME) *client_ca_list;
	liValue *v;

	/* setup defaults */
	GString *ipstr = NULL;
	const char
		*ciphers = "ECDHE-RSA-AES256-SHA384:AES256-SHA256:RC4-SHA:RC4:HIGH:!MD5:!aNULL:!EDH:!AESGCM",
		*pemfile = NULL, *ca_file = NULL, *client_ca_file = NULL;
	long
		options = SSL_OP_NO_SSLv2 | SSL_OP_CIPHER_SERVER_PREFERENCE
#ifdef SSL_OP_NO_COMPRESSION
			| SSL_OP_NO_COMPRESSION
#endif
		;
	guint
		verify_mode = 0, verify_depth = 1;
	gboolean
		verify_any = FALSE;

	UNUSED(p); UNUSED(userdata);

	if (val->type != LI_VALUE_HASH) {
		ERROR(srv, "%s", "openssl expects a hash as parameter");
		return FALSE;
	}

	g_hash_table_iter_init(&hti, val->data.hash);
	while (g_hash_table_iter_next(&hti, &hkey, &hvalue)) {
		htkey = hkey; htval = hvalue;

		if (g_str_equal(htkey->str, "listen")) {
			if (htval->type != LI_VALUE_STRING) {
				ERROR(srv, "%s", "openssl listen expects a string as parameter");
				return FALSE;
			}
			ipstr = htval->data.string;
		} else if (g_str_equal(htkey->str, "pemfile")) {
			if (htval->type != LI_VALUE_STRING) {
				ERROR(srv, "%s", "openssl pemfile expects a string as parameter");
				return FALSE;
			}
			pemfile = htval->data.string->str;
		} else if (g_str_equal(htkey->str, "ca-file")) {
			if (htval->type != LI_VALUE_STRING) {
				ERROR(srv, "%s", "openssl ca-file expects a string as parameter");
				return FALSE;
			}
			ca_file = htval->data.string->str;
		} else if (g_str_equal(htkey->str, "ciphers")) {
			if (htval->type != LI_VALUE_STRING) {
				ERROR(srv, "%s", "openssl ciphers expects a string as parameter");
				return FALSE;
			}
			ciphers = htval->data.string->str;
		} else if (g_str_equal(htkey->str, "options")) {
			guint i;

			if (htval->type != LI_VALUE_LIST) {
				ERROR(srv, "%s", "openssl options expects a list of strings as parameter");
				return FALSE;
			}
			for (i = 0; i < htval->data.list->len; i++) {
				v = g_array_index(htval->data.list, liValue*, i);
				if (v->type != LI_VALUE_STRING) {
					ERROR(srv, "%s", "openssl options expects a list of strings as parameter");
					return FALSE;
				}
				if (!openssl_options_set_string(&options, v->data.string)) {
					ERROR(srv, "openssl option unknown: %s", v->data.string->str);
					return FALSE;
				}
			}
		} else if (g_str_equal(htkey->str, "verify")) {
			if (htval->type != LI_VALUE_BOOLEAN) {
				ERROR(srv, "%s", "openssl verify expects a boolean as parameter");
				return FALSE;
			}
			if (htval->data.boolean)
				verify_mode |= SSL_VERIFY_PEER;
		} else if (g_str_equal(htkey->str, "verify-any")) {
			if (htval->type != LI_VALUE_BOOLEAN) {
				ERROR(srv, "%s", "openssl verify-any expects a boolean as parameter");
				return FALSE;
			}
			verify_any = htval->data.boolean;
		} else if (g_str_equal(htkey->str, "verify-depth")) {
			if (htval->type != LI_VALUE_NUMBER) {
				ERROR(srv, "%s", "openssl verify-depth expects a number as parameter");
				return FALSE;
			}
			verify_depth = htval->data.number;
		} else if (g_str_equal(htkey->str, "verify-require")) {
			if (htval->type != LI_VALUE_BOOLEAN) {
				ERROR(srv, "%s", "openssl verify-require expects a boolean as parameter");
				return FALSE;
			}
			if (htval->data.boolean)
				verify_mode |= SSL_VERIFY_FAIL_IF_NO_PEER_CERT;
		} else if (g_str_equal(htkey->str, "client-ca-file")) {
			if (htval->type != LI_VALUE_STRING) {
				ERROR(srv, "%s", "openssl client-ca-file expects a string as parameter");
				return FALSE;
			}
			client_ca_file = htval->data.string->str;
		}
	}

	if (!ipstr) {
		ERROR(srv, "%s", "openssl needs a listen parameter");
		return FALSE;
	}

	if (!pemfile) {
		ERROR(srv, "%s", "openssl needs a pemfile");
		return FALSE;
	}

	ctx = g_slice_new0(openssl_context);

	if (NULL == (ctx->ssl_ctx = SSL_CTX_new(SSLv23_server_method()))) {
		ERROR(srv, "SSL_CTX_new: %s", ERR_error_string(ERR_get_error(), NULL));
		goto error_free_socket;
	}

	if (!SSL_CTX_set_options(ctx->ssl_ctx, options)) {
		ERROR(srv, "SSL_CTX_set_options(%lx): %s", options, ERR_error_string(ERR_get_error(), NULL));
		goto error_free_socket;
	}

	if (ciphers) {
		/* Disable support for low encryption ciphers */
		if (SSL_CTX_set_cipher_list(ctx->ssl_ctx, ciphers) != 1) {
			ERROR(srv, "SSL_CTX_set_cipher_list('%s'): %s", ciphers, ERR_error_string(ERR_get_error(), NULL));
			goto error_free_socket;
		}
	}

	if (ca_file) {
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

	if (client_ca_file) {
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

	li_angel_listen(srv, ipstr, openssl_setup_listen_cb, ctx);

	return TRUE;

error_free_socket:
	if (ctx) {
		if (ctx->ssl_ctx) SSL_CTX_free(ctx->ssl_ctx);
		g_slice_free(openssl_context, ctx);
	}

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

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
 *       options        - (list of strings) set OpenSSL-specific options (default: NO_SSLv2, CIPHER_SERVER_PREFERENCE)
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
 *     Copyright (c) 2009 Stefan Bühler, Joe Presbrey
 */

#include <lighttpd/base.h>
#include <lighttpd/plugin_core.h>

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/rand.h>

LI_API gboolean mod_openssl_init(liModules *mods, liModule *mod);
LI_API gboolean mod_openssl_free(liModules *mods, liModule *mod);


typedef struct openssl_connection_ctx openssl_connection_ctx;
typedef struct openssl_context openssl_context;

struct openssl_connection_ctx {
	SSL *ssl;
	liConnection *con;

	int con_events;
	liJob con_handle_events_job;
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

static const char openssl_setenv_config_error[] = "openssl.setenv expects a string or a list of strings consisting of: client, client-cert, server, server-cert";

static void openssl_con_handle_events_cb(liJob *job) {
	openssl_connection_ctx *conctx = LI_CONTAINER_OF(job, openssl_connection_ctx, con_handle_events_job);
	liConnection *con = conctx->con;

	connection_handle_io(con);
}

static void openssl_io_cb(struct ev_loop *loop, ev_io *w, int revents) {
	liConnection *con = (liConnection*) w->data;
	openssl_connection_ctx *conctx = con->srv_sock_data;

	if (revents & EV_ERROR) {
		/* if this happens, we have a serious bug in the event handling */
		VR_ERROR(con->mainvr, "%s", "EV_ERROR encountered, dropping connection!");
		li_connection_error(con);
		return;
	}

	con->can_read = TRUE;
	con->can_write = TRUE;

	/* disable all events; they will get reactivated later */
	li_ev_io_set_events(loop, w, 0);

	li_job_now(&con->wrk->jobqueue, &conctx->con_handle_events_job);
}

static gboolean openssl_con_new(liConnection *con) {
	liServer *srv = con->srv;
	openssl_context *ctx = con->srv_sock->data;
	openssl_connection_ctx *conctx = g_slice_new0(openssl_connection_ctx);

	con->srv_sock_data = NULL;

	conctx->ssl = SSL_new(ctx->ssl_ctx);
	conctx->con = con;
	li_job_init(&conctx->con_handle_events_job, openssl_con_handle_events_cb);
	conctx->con_events = 0;

	if (NULL == conctx->ssl) {
		ERROR(srv, "SSL_new: %s", ERR_error_string(ERR_get_error(), NULL));
		goto fail;
	}

	SSL_set_accept_state(conctx->ssl);

	if (1 != (SSL_set_fd(conctx->ssl, con->sock_watcher.fd))) {
		ERROR(srv, "SSL_set_fd: %s", ERR_error_string(ERR_get_error(), NULL));
		goto fail;
	}

	con->srv_sock_data = conctx;
	con->info.is_ssl = TRUE;

	ev_set_cb(&con->sock_watcher, openssl_io_cb);

	return TRUE;

fail:
	if (conctx->ssl) {
		SSL_free(conctx->ssl);
	}

	g_slice_free(openssl_connection_ctx, conctx);

	return FALSE;
}

static void openssl_con_close(liConnection *con) {
	openssl_connection_ctx *conctx = con->srv_sock_data;

	if (!conctx) return;

	if (conctx->ssl) {
		SSL_shutdown(conctx->ssl); /* TODO: wait for something??? */
		SSL_free(conctx->ssl);
		conctx->ssl = FALSE;
	}

	con->srv_sock_data = NULL;
	con->info.is_ssl = FALSE;
	li_job_clear(&conctx->con_handle_events_job);

	g_slice_free(openssl_connection_ctx, conctx);
}

static void openssl_update_events(liConnection *con, int events) {
	openssl_connection_ctx *conctx = con->srv_sock_data;

	/* new events -> add them to socket watcher too */
	if (0 != (events & ~conctx->con_events)) {
		li_ev_io_add_events(con->wrk->loop, &con->sock_watcher, events);
	}

	conctx->con_events = events;
}

static liNetworkStatus openssl_con_write(liConnection *con, goffset write_max) {
	const ssize_t blocksize = 16*1024; /* 16k */
	char *block_data;
	off_t block_len;
	ssize_t r;
	liChunkIter ci;
	liChunkQueue *cq = con->raw_out;
	openssl_connection_ctx *conctx = con->srv_sock_data;

	do {
		if (0 == cq->length)
			return LI_NETWORK_STATUS_SUCCESS;

		ci = li_chunkqueue_iter(cq);
		switch (li_chunkiter_read(con->mainvr, ci, 0, blocksize, &block_data, &block_len)) {
		case LI_HANDLER_GO_ON:
			break;
		case LI_HANDLER_ERROR:
		default:
			return LI_NETWORK_STATUS_FATAL_ERROR;
		}

		/**
		 * SSL_write man-page
		 *
		 * WARNING
		 *        When an SSL_write() operation has to be repeated because of
		 *        SSL_ERROR_WANT_READ or SSL_ERROR_WANT_WRITE, it must be
		 *        repeated with the same arguments.
		 *
		 */

		ERR_clear_error();
		if ((r = SSL_write(conctx->ssl, block_data, block_len)) <= 0) {
			unsigned long err;

			switch (SSL_get_error(conctx->ssl, r)) {
			case SSL_ERROR_WANT_READ:
				li_ev_io_add_events(con->wrk->loop, &con->sock_watcher, EV_READ);
				return LI_NETWORK_STATUS_WAIT_FOR_EVENT;
			case SSL_ERROR_WANT_WRITE:
				li_ev_io_add_events(con->wrk->loop, &con->sock_watcher, EV_WRITE);
				return LI_NETWORK_STATUS_WAIT_FOR_EVENT;
			case SSL_ERROR_SYSCALL:
				/* perhaps we have error waiting in our error-queue */
				if (0 != (err = ERR_get_error())) {
					do {
						VR_ERROR(con->mainvr, "SSL_write(%i): %s",
							con->sock_watcher.fd,
							ERR_error_string(err, NULL));
					} while (0 != (err = ERR_get_error()));
				} else if (r == -1) {
					/* no, but we have errno */
					switch(errno) {
					case EPIPE:
					case ECONNRESET:
						return LI_NETWORK_STATUS_CONNECTION_CLOSE;
					default:
						VR_ERROR(con->mainvr, "SSL_write(%i): %s",
							con->sock_watcher.fd,
							g_strerror(errno));
						break;
					}
				} else {
					/* neither error-queue nor errno ? */
					VR_ERROR(con->mainvr, "SSL_write(%i): %s",
						con->sock_watcher.fd,
						"Unexpected eof");
					return LI_NETWORK_STATUS_CONNECTION_CLOSE;
				}

				return LI_NETWORK_STATUS_FATAL_ERROR;
			case SSL_ERROR_ZERO_RETURN:
				/* clean shutdown on the remote side */
				return LI_NETWORK_STATUS_CONNECTION_CLOSE;
			default:
				while (0 != (err = ERR_get_error())) {
					VR_ERROR(con->mainvr, "SSL_write(%i): %s",
						con->sock_watcher.fd,
						ERR_error_string(err, NULL));
				}

				return LI_NETWORK_STATUS_FATAL_ERROR;
			}
		}

		li_chunkqueue_skip(cq, r);
		write_max -= r;
	} while (r == block_len && write_max > 0);

	if (0 != cq->length) {
		li_ev_io_add_events(con->wrk->loop, &con->sock_watcher, EV_WRITE);
	}
	return LI_NETWORK_STATUS_SUCCESS;
}

static liNetworkStatus openssl_con_read(liConnection *con) {
	liChunkQueue *cq = con->raw_in;
	openssl_connection_ctx *conctx = con->srv_sock_data;

	const ssize_t blocksize = 16*1024; /* 16k */
	off_t max_read = 16 * blocksize; /* 256k */
	ssize_t r;
	off_t len = 0;

	if (cq->limit && cq->limit->limit > 0) {
		if (max_read > cq->limit->limit - cq->limit->current) {
			max_read = cq->limit->limit - cq->limit->current;
			if (max_read <= 0) {
				max_read = 0; /* we still have to read something */
				VR_ERROR(con->mainvr, "li_network_read: fd %i should be disabled as chunkqueue is already full",
					con->sock_watcher.fd);
			}
		}
	}

	do {
		liBuffer *buf;
		gboolean cq_buf_append;

		ERR_clear_error();

		buf = li_chunkqueue_get_last_buffer(cq, 1024);
		cq_buf_append = (buf != NULL);

		if (buf != NULL) {
			/* use last buffer as raw_in_buffer; they should be the same anyway */
			if (G_UNLIKELY(buf != con->raw_in_buffer)) {
				li_buffer_acquire(buf);
				li_buffer_release(con->raw_in_buffer);
				con->raw_in_buffer = buf;
			}
		} else {
			buf = con->raw_in_buffer;
			if (buf != NULL && buf->alloc_size - buf->used < 1024) {
				/* release *buffer */
				li_buffer_release(buf);
				con->raw_in_buffer = buf = NULL;
			}
			if (buf == NULL) {
				con->raw_in_buffer = buf = li_buffer_new(blocksize);
			}
		}
		assert(con->raw_in_buffer == buf);

		r = SSL_read(conctx->ssl, buf->addr + buf->used, buf->alloc_size - buf->used);
		if (r < 0) {
			int oerrno = errno, err;
			gboolean was_fatal;

			err = SSL_get_error(conctx->ssl, r);

			switch (err) {
			case SSL_ERROR_WANT_READ:
				li_ev_io_add_events(con->wrk->loop, &con->sock_watcher, EV_READ);
				/* ignore requirement that we should pass the same buffer again */
				return (len > 0) ? LI_NETWORK_STATUS_SUCCESS : LI_NETWORK_STATUS_WAIT_FOR_EVENT;
			case SSL_ERROR_WANT_WRITE:
				li_ev_io_add_events(con->wrk->loop, &con->sock_watcher, EV_WRITE);
				/* ignore requirement that we should pass the same buffer again */
				return (len > 0) ? LI_NETWORK_STATUS_SUCCESS : LI_NETWORK_STATUS_WAIT_FOR_EVENT;
			default:
				break;
			}

			switch (err) {
			case SSL_ERROR_SYSCALL:
				/**
				 * man SSL_get_error()
				 *
				 * SSL_ERROR_SYSCALL
				 *   Some I/O error occurred.  The OpenSSL error queue may contain more
				 *   information on the error.  If the error queue is empty (i.e.
				 *   ERR_get_error() returns 0), ret can be used to find out more about
				 *   the error: If ret == 0, an EOF was observed that violates the
				 *   protocol.  If ret == -1, the underlying BIO reported an I/O error
				 *   (for socket I/O on Unix systems, consult errno for details).
				 *
				 */
				while (0 != (err = ERR_get_error())) {
					VR_ERROR(con->mainvr, "SSL_read(%i): %s",
						con->sock_watcher.fd,
						ERR_error_string(err, NULL));
				}

				switch (oerrno) {
				case EPIPE:
				case ECONNRESET:
					return LI_NETWORK_STATUS_CONNECTION_CLOSE;
				}

				VR_ERROR(con->mainvr, "SSL_read(%i): %s",
					con->sock_watcher.fd,
					g_strerror(oerrno));

				break;
			case SSL_ERROR_ZERO_RETURN:
				/* clean shutdown on the remote side */
				return LI_NETWORK_STATUS_CONNECTION_CLOSE;
			default:
				was_fatal = FALSE;

				while((err = ERR_get_error())) {
					switch (ERR_GET_REASON(err)) {
					case SSL_R_SSL_HANDSHAKE_FAILURE:
					case SSL_R_TLSV1_ALERT_UNKNOWN_CA:
					case SSL_R_SSLV3_ALERT_CERTIFICATE_UNKNOWN:
					case SSL_R_SSLV3_ALERT_BAD_CERTIFICATE:
						/* TODO: if (!con->conf.log_ssl_noise) */ continue;
						break;
					default:
						was_fatal = TRUE;
						break;
					}
					/* get all errors from the error-queue */
					VR_ERROR(con->mainvr, "SSL_read(%i): %s",
						con->sock_watcher.fd,
						ERR_error_string(err, NULL));
				}
				if (!was_fatal) return LI_NETWORK_STATUS_CONNECTION_CLOSE;
				break;
			}

			return LI_NETWORK_STATUS_FATAL_ERROR;
		} else if (r == 0) {
			return LI_NETWORK_STATUS_CONNECTION_CLOSE;
		}

		if (cq_buf_append) {
			li_chunkqueue_update_last_buffer_size(cq, r);
		} else {
			gsize offset;

			li_buffer_acquire(buf);

			offset = buf->used;
			buf->used += r;
			li_chunkqueue_append_buffer2(cq, buf, offset, r);
		}
		if (buf->alloc_size - buf->used < 1024) {
			/* release *buffer */
			li_buffer_release(buf);
			con->raw_in_buffer = buf = NULL;
		}
		len += r;
	} while (len < max_read);

	return LI_NETWORK_STATUS_SUCCESS;
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
		|| !(conctx = con->srv_sock_data)
		|| !(ssl = conctx->ssl))
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

	srv_sock->write_cb = openssl_con_write;
	srv_sock->read_cb = openssl_con_read;
	srv_sock->new_cb = openssl_con_new;
	srv_sock->close_cb = openssl_con_close;
	srv_sock->release_cb = openssl_sock_release;
	srv_sock->update_events_cb = openssl_update_events;
}

static int openssl_options_set_string(guint options, GString *s) {
	struct {
		char *name;
		long value;
	} option_string[] = {
		{"MICROSOFT_SESS_ID_BUG", SSL_OP_MICROSOFT_SESS_ID_BUG},
		{"NETSCAPE_CHALLENGE_BUG", SSL_OP_NETSCAPE_CHALLENGE_BUG},
#ifdef SSL_OP_LEGACY_SERVER_CONNECT
		{"LEGACY_SERVER_CONNECT", SSL_OP_LEGACY_SERVER_CONNECT},
#endif
		{"NETSCAPE_REUSE_CIPHER_CHANGE_BUG", SSL_OP_NETSCAPE_REUSE_CIPHER_CHANGE_BUG},
		{"SSLREF2_REUSE_CERT_TYPE_BUG", SSL_OP_SSLREF2_REUSE_CERT_TYPE_BUG},
		{"MICROSOFT_BIG_SSLV3_BUFFER", SSL_OP_MICROSOFT_BIG_SSLV3_BUFFER},
		{"MSIE_SSLV2_RSA_PADDING", SSL_OP_MSIE_SSLV2_RSA_PADDING},
		{"SSLEAY_080_CLIENT_DH_BUG", SSL_OP_SSLEAY_080_CLIENT_DH_BUG},
		{"TLS_D5_BUG", SSL_OP_TLS_D5_BUG},
		{"TLS_BLOCK_PADDING_BUG", SSL_OP_TLS_BLOCK_PADDING_BUG},
#ifdef SSL_OP_DONT_INSERT_EMPTY_FRAGMENTS
		{"DONT_INSERT_EMPTY_FRAGMENTS", SSL_OP_DONT_INSERT_EMPTY_FRAGMENTS},
#endif
		{"ALL", SSL_OP_ALL},
#ifdef SSL_OP_NO_QUERY_MTU
		{"NO_QUERY_MTU", SSL_OP_NO_QUERY_MTU},
#endif
#ifdef SSL_OP_COOKIE_EXCHANGE
		{"COOKIE_EXCHANGE", SSL_OP_COOKIE_EXCHANGE},
#endif
#ifdef SSL_OP_NO_TICKET
		{"NO_TICKET", SSL_OP_NO_TICKET},
#endif
#ifdef SSL_OP_CISCO_ANYCONNECT
		{"CISCO_ANYCONNECT", SSL_OP_CISCO_ANYCONNECT},
#endif
#ifdef SSL_OP_NO_SESSION_RESUMPTION_ON_RENEGOTIATION
		{"NO_SESSION_RESUMPTION_ON_RENEGOTIATION", SSL_OP_NO_SESSION_RESUMPTION_ON_RENEGOTIATION},
#endif
#ifdef SSL_OP_NO_COMPRESSION
		{"NO_COMPRESSION", SSL_OP_NO_COMPRESSION},
#endif
#ifdef SSL_OP_ALLOW_UNSAFE_LEGACY_RENEGOTIATION
		{"ALLOW_UNSAFE_LEGACY_RENEGOTIATION", SSL_OP_ALLOW_UNSAFE_LEGACY_RENEGOTIATION},
#endif
#ifdef SSL_OP_SINGLE_ECDH_USE
		{"SINGLE_ECDH_USE", SSL_OP_SINGLE_ECDH_USE},
#endif
		{"SINGLE_DH_USE", SSL_OP_SINGLE_DH_USE},
		{"EPHEMERAL_RSA", SSL_OP_EPHEMERAL_RSA},
#ifdef SSL_OP_CIPHER_SERVER_PREFERENCE
		{"CIPHER_SERVER_PREFERENCE", SSL_OP_CIPHER_SERVER_PREFERENCE},
#endif
		{"TLS_ROLLBACK_BUG", SSL_OP_TLS_ROLLBACK_BUG},
		{"NO_SSLv2", SSL_OP_NO_SSLv2},
		{"NO_SSLv3", SSL_OP_NO_SSLv3},
		{"NO_TLSv1", SSL_OP_NO_TLSv1},
		{"PKCS1_CHECK_1", SSL_OP_PKCS1_CHECK_1},
		{"PKCS1_CHECK_2", SSL_OP_PKCS1_CHECK_2},
		{"NETSCAPE_CA_DN_BUG", SSL_OP_NETSCAPE_CA_DN_BUG},
		{"NETSCAPE_DEMO_CIPHER_CHANGE_BUG", SSL_OP_NETSCAPE_DEMO_CIPHER_CHANGE_BUG},
#ifdef SSL_OP_CRYPTOPRO_TLSEXT_BUG
		{"CRYPTOPRO_TLSEXT_BUG", SSL_OP_CRYPTOPRO_TLSEXT_BUG},
#endif
		{NULL, 0}
	}, *option;
	if (0 == g_ascii_strncasecmp(s->str, CONST_STR_LEN("SSLv2")))
		options &= ~SSL_OP_NO_SSLv2;
	else if (0 == g_ascii_strncasecmp(s->str, CONST_STR_LEN("NO_CIPHER_SERVER_PREFERENCE")))
		options &= ~SSL_OP_CIPHER_SERVER_PREFERENCE;
	else
		for (option = option_string; option->name; ++option)
			if (0 == g_ascii_strncasecmp(s->str, option->name, strlen(option->name))) {
				options |= option->value;
				break;
			}
	return options;
}

static int openssl_verify_any_cb(int ok, X509_STORE_CTX *ctx) { UNUSED(ok); UNUSED(ctx); return 1; }

static gboolean openssl_setup(liServer *srv, liPlugin* p, liValue *val, gpointer userdata) {
	openssl_context *ctx;
	GHashTableIter hti;
	gpointer hkey, hvalue;
	GString *htkey;
	liValue *htval;
	STACK_OF(X509_NAME) *client_ca_list;
	guint i, j;
	liValue *v;

	/* setup defaults */
	GString *ipstr = NULL;
	const char
		*ciphers = "ECDHE-RSA-AES256-SHA384:AES256-SHA256:RC4-SHA:RC4:HIGH:!MD5:!aNULL:!EDH:!AESGCM:!SSLv2",
		*pemfile = NULL, *ca_file = NULL, *client_ca_file = NULL;
	guint
		options = SSL_OP_NO_SSLv2 | SSL_OP_CIPHER_SERVER_PREFERENCE,
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
				ERROR(srv, "%s", "openssl pemfile expects a string as parameter");
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
				if (options == (j = openssl_options_set_string(options, v->data.string))) {
					ERROR(srv, "openssl option unknown: %s", v->data.string->str);
					return FALSE;
				}
				options = j;
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
		ERROR(srv, "SSL_CTX_set_options(%x): %s", options, ERR_error_string(ERR_get_error(), NULL));
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

static void sslthread_init() {
	int n = CRYPTO_num_locks(), i;

	ssl_locks = g_slice_alloc0(sizeof(GMutex*) * n);

	for (i = 0; i < n; i++) {
		ssl_locks[i] = g_mutex_new();
	}

	CRYPTO_set_locking_callback(ssl_lock_cb);
	CRYPTO_set_id_callback(ssl_id_cb);
}

static void sslthread_free() {
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

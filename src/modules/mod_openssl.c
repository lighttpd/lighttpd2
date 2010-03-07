/*
 * mod_openssl - ssl support
 *
 * Description:
 *     mod_openssl listens on separate sockets for ssl connections (https://...)
 *
 * Setups:
 *     openssl    - setup a ssl socket; takes a hash of following parameters:
 *       listen     - (mandatory) the socket address (same as standard listen)
 *       pemfile    - (mandatory) contains key and direct certificate for the key (PEM format)
 *       ca-file    - contains certificate chain
 *       ciphers    - contains colon separated list of allowed ciphers
 *       allow-ssl2 - boolean option to allow ssl2 (disabled by default)
 *
 * Example config:
 *     setup openssl [ "listen": "0.0.0.0:8443", "pemfile": "server.pem" ];
 *     setup openssl [ "listen": "[::]:8443", "pemfile": "server.pem" ];
 *
 * Author:
 *     Copyright (c) 2009 Stefan Bühler
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
};

struct openssl_context {
	SSL_CTX *ssl_ctx;
};

static gboolean openssl_con_new(liConnection *con) {
	liServer *srv = con->srv;
	openssl_context *ctx = con->srv_sock->data;
	openssl_connection_ctx *conctx = g_slice_new0(openssl_connection_ctx);

	con->srv_sock_data = NULL;

	conctx->ssl = SSL_new(ctx->ssl_ctx);

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
	con->is_ssl = TRUE;

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
	con->is_ssl = FALSE;

	g_slice_free(openssl_connection_ctx, conctx);
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
			int ssl_r;
			unsigned long err;

			switch ((ssl_r = SSL_get_error(conctx->ssl, r))) {
			case SSL_ERROR_WANT_READ:
			case SSL_ERROR_WANT_WRITE:
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

			if (SSL_ERROR_WANT_READ == err || SSL_ERROR_WANT_WRITE == err) {
				/* ignore requirement that we should pass the same buffer again */
				return (len > 0) ? LI_NETWORK_STATUS_SUCCESS : LI_NETWORK_STATUS_WAIT_FOR_EVENT;
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
}

static gboolean openssl_setup(liServer *srv, liPlugin* p, liValue *val, gpointer userdata) {
	openssl_context *ctx;
	GHashTableIter hti;
	gpointer hkey, hvalue;
	GString *htkey;
	liValue *htval;

	/* options */
	const char *pemfile = NULL, *ca_file = NULL, *ciphers = NULL;
	GString *ipstr = NULL;
	gboolean allow_ssl2 = FALSE;

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
		} else if (g_str_equal(htkey->str, "allow-ssl2")) {
			if (htval->type != LI_VALUE_BOOLEAN) {
				ERROR(srv, "%s", "openssl allow-ssl2 expects a boolean as parameter");
				return FALSE;
			}
			allow_ssl2 = htval->data.boolean;
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

	if (!allow_ssl2) {
		/* disable SSLv2 */
		if (0 == (SSL_OP_NO_SSLv2 & SSL_CTX_set_options(ctx->ssl_ctx, SSL_OP_NO_SSLv2))) {
			ERROR(srv, "SSL_CTX_set_options(SSL_OP_NO_SSLv2): %s", ERR_error_string(ERR_get_error(), NULL));
			goto error_free_socket;
		}
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

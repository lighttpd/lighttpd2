
#include <lighttpd/base.h>

#include <gnutls/gnutls.h>
#include <glib-2.0/glib/galloca.h>


LI_API gboolean mod_gnutls_init(liModules *mods, liModule *mod);
LI_API gboolean mod_gnutls_free(liModules *mods, liModule *mod);


typedef struct mod_connection_ctx mod_connection_ctx;
typedef struct mod_context mod_context;

struct mod_connection_ctx {
	gnutls_session_t session;
	liConnection *con;
	mod_context *ctx;

	int con_events;
	liJob con_handle_events_job;

	unsigned int gnutls_again_in_progress:1;

	unsigned int initial_handshaked_finished:1;
	unsigned int client_initiated_renegotiation:1;
};

struct mod_context {
	gint refcount;

	gnutls_certificate_credentials_t server_cert;
	gnutls_priority_t server_priority;
	gnutls_priority_t server_priority_beast;

	unsigned int protect_against_beast:1;
};

static void mod_gnutls_context_release(mod_context *ctx) {
	if (!ctx) return;
	assert(g_atomic_int_get(&ctx->refcount) > 0);
	if (g_atomic_int_dec_and_test(&ctx->refcount)) {
		gnutls_priority_deinit(ctx->server_priority_beast);
		gnutls_priority_deinit(ctx->server_priority);
		gnutls_certificate_free_credentials(ctx->server_cert);

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

	ctx->refcount = 1;
	ctx->protect_against_beast = 1;

	return ctx;

error2:
	gnutls_priority_deinit(ctx->server_priority);

error1:
	gnutls_certificate_free_credentials(ctx->server_cert);

error0:
	g_slice_free(mod_context, ctx);
	return NULL;
}




static void mod_gnutls_con_handle_events_cb(liJob *job) {
	mod_connection_ctx *conctx = LI_CONTAINER_OF(job, mod_connection_ctx, con_handle_events_job);
	liConnection *con = conctx->con;

	conctx->gnutls_again_in_progress = 0;
	connection_handle_io(con);
}

static void mod_gnutls_io_cb(struct ev_loop *loop, ev_io *w, int revents) {
	liConnection *con = (liConnection*) w->data;
	mod_connection_ctx *conctx = con->srv_sock_data;

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

static int mod_gnutls_post_client_hello_cb(gnutls_session_t session) {
	gnutls_protocol_t p = gnutls_protocol_get_version(session);
	mod_connection_ctx *conctx = gnutls_session_get_ptr(session);

	if (conctx->ctx->protect_against_beast) {
		if (GNUTLS_SSL3 == p || GNUTLS_TLS1_0 == p) {
			gnutls_priority_set(session, conctx->ctx->server_priority_beast);
		}
	}

	return GNUTLS_E_SUCCESS;
}

static gboolean mod_gnutls_con_new(liConnection *con) {
	liServer *srv = con->srv;
	mod_context *ctx = con->srv_sock->data;
	mod_connection_ctx *conctx = g_slice_new0(mod_connection_ctx);
	int r;

	con->srv_sock_data = NULL;

	if (GNUTLS_E_SUCCESS != (r = gnutls_init(&conctx->session, GNUTLS_SERVER))) {
		ERROR(srv, "gnutls_init (%s): %s",
			gnutls_strerror_name(r), gnutls_strerror(r));
		g_slice_free(mod_connection_ctx, conctx);
		return FALSE;
	}

	mod_gnutls_context_acquire(ctx);

	if (GNUTLS_E_SUCCESS != (r = gnutls_priority_set(conctx->session, ctx->server_priority))) {
		ERROR(srv, "gnutls_priority_set (%s): %s",
			gnutls_strerror_name(r), gnutls_strerror(r));
		goto fail;
	}
	if (GNUTLS_E_SUCCESS != (r = gnutls_credentials_set(conctx->session, GNUTLS_CRD_CERTIFICATE, ctx->server_cert))) {
		ERROR(srv, "gnutls_credentials_set (%s): %s",
			gnutls_strerror_name(r), gnutls_strerror(r));
		goto fail;
	}

	gnutls_transport_set_ptr(conctx->session, (gnutls_transport_ptr_t)(intptr_t) con->sock_watcher.fd);
	gnutls_session_set_ptr(conctx->session, conctx);

	gnutls_handshake_set_post_client_hello_function(conctx->session, mod_gnutls_post_client_hello_cb);

	conctx->con = con;
	conctx->ctx = ctx;
	li_job_init(&conctx->con_handle_events_job, mod_gnutls_con_handle_events_cb);
	conctx->con_events = 0;
	conctx->gnutls_again_in_progress = 0;
	conctx->initial_handshaked_finished = 0;
	conctx->client_initiated_renegotiation = 0;

	con->srv_sock_data = conctx;
	con->info.is_ssl = TRUE;

	ev_set_cb(&con->sock_watcher, mod_gnutls_io_cb);

	return TRUE;

fail:
	gnutls_deinit(conctx->session);
	mod_gnutls_context_release(ctx);

	g_slice_free(mod_connection_ctx, conctx);

	return FALSE;
}

static void mod_gnutls_con_close(liConnection *con) {
	mod_connection_ctx *conctx = con->srv_sock_data;

	if (!conctx) return;

	gnutls_bye(conctx->session, GNUTLS_SHUT_RDWR);
	gnutls_deinit(conctx->session);
	mod_gnutls_context_release(conctx->ctx);

	con->srv_sock_data = NULL;
	con->info.is_ssl = FALSE;
	li_job_clear(&conctx->con_handle_events_job);

	g_slice_free(mod_connection_ctx, conctx);
}

static void mod_gnutls_update_events(liConnection *con, int events) {
	mod_connection_ctx *conctx = con->srv_sock_data;

	/* new events -> add them to socket watcher too */
	if (!conctx->gnutls_again_in_progress && 0 != (events & ~conctx->con_events)) {
		li_ev_io_add_events(con->wrk->loop, &con->sock_watcher, events);
	}

	conctx->con_events = events;
}

static liNetworkStatus mod_gnutls_handle_error(liConnection *con, mod_connection_ctx *conctx, const char *gnutlsfunc, off_t len, int r) {
	switch (r) {
	case GNUTLS_E_AGAIN:
		conctx->gnutls_again_in_progress = TRUE;
		li_ev_io_set_events(con->wrk->loop, &con->sock_watcher, gnutls_record_get_direction(conctx->session) ? EV_WRITE : EV_READ);
		return (len > 0) ? LI_NETWORK_STATUS_SUCCESS : LI_NETWORK_STATUS_WAIT_FOR_EVENT;
	case GNUTLS_E_REHANDSHAKE:
		if (conctx->initial_handshaked_finished) {
			VR_ERROR(con->mainvr, "%s", "gnutls: client initiated renegotitation, closing connection");
			return LI_NETWORK_STATUS_FATAL_ERROR;
		}
		break;
	case GNUTLS_E_UNEXPECTED_PACKET_LENGTH:
		/* connection abort */
		return LI_NETWORK_STATUS_CONNECTION_CLOSE;
	case GNUTLS_E_UNKNOWN_CIPHER_SUITE:
	case GNUTLS_E_UNSUPPORTED_VERSION_PACKET:
		VR_DEBUG(con->mainvr, "%s (%s): %s", gnutlsfunc,
			gnutls_strerror_name(r), gnutls_strerror(r));
		return LI_NETWORK_STATUS_CONNECTION_CLOSE;
	default:
		if (gnutls_error_is_fatal(r)) {
			VR_ERROR(con->mainvr, "%s (%s): %s", gnutlsfunc,
				gnutls_strerror_name(r), gnutls_strerror(r));
			return LI_NETWORK_STATUS_FATAL_ERROR;
		} else {
			VR_ERROR(con->mainvr, "%s non fatal (%s): %s", gnutlsfunc,
				gnutls_strerror_name(r), gnutls_strerror(r));
		}
	}
	return (len > 0) ? LI_NETWORK_STATUS_SUCCESS : LI_NETWORK_STATUS_WAIT_FOR_EVENT;
}

static liNetworkStatus mod_gnutls_do_handshake(liConnection *con, mod_connection_ctx *conctx) {
	int r;

	if (GNUTLS_E_SUCCESS != (r = gnutls_handshake(conctx->session))) {
		return mod_gnutls_handle_error(con, conctx, "gnutls_handshake", 0, r);
	} else {
		conctx->initial_handshaked_finished = 1;
		return LI_NETWORK_STATUS_SUCCESS;
	}
}

static liNetworkStatus mod_gnutls_con_write(liConnection *con, goffset write_max) {
	const ssize_t blocksize = 16*1024; /* 16k */
	char *block_data;
	off_t block_len;
	ssize_t r;
	liChunkIter ci;
	liChunkQueue *cq = con->raw_out;
	mod_connection_ctx *conctx = con->srv_sock_data;

	if (!conctx->initial_handshaked_finished) {
		liNetworkStatus res = mod_gnutls_do_handshake(con, conctx);
		if (res != LI_NETWORK_STATUS_SUCCESS) return res;
	}

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

		if (0 >= (r = gnutls_record_send(conctx->session, block_data, block_len))) {
			return mod_gnutls_handle_error(con, conctx, "gnutls_record_send", 0, r);
		}

		/* VR_BACKEND_LINES(con->mainvr, block_data, "written %i from %i bytes: ", (int) r, (int) block_len); */

		li_chunkqueue_skip(cq, r);
		write_max -= r;
	} while (r == block_len && write_max > 0);

	if (0 != cq->length) {
		li_ev_io_add_events(con->wrk->loop, &con->sock_watcher, EV_WRITE);
	}
	return LI_NETWORK_STATUS_SUCCESS;
}

static liNetworkStatus mod_gnutls_con_read(liConnection *con) {
	liChunkQueue *cq = con->raw_in;
	mod_connection_ctx *conctx = con->srv_sock_data;

	const ssize_t blocksize = 16*1024; /* 16k */
	off_t max_read = 16 * blocksize; /* 256k */
	ssize_t r;
	off_t len = 0;

	if (!conctx->initial_handshaked_finished) {
		liNetworkStatus res = mod_gnutls_do_handshake(con, conctx);
		if (res != LI_NETWORK_STATUS_SUCCESS) return res;
	}

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

		if (0 > (r = gnutls_record_recv(conctx->session, buf->addr + buf->used, buf->alloc_size - buf->used))) {
			return mod_gnutls_handle_error(con, conctx, "gnutls_record_recv", len, r);
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

	srv_sock->write_cb = mod_gnutls_con_write;
	srv_sock->read_cb = mod_gnutls_con_read;
	srv_sock->new_cb = mod_gnutls_con_new;
	srv_sock->close_cb = mod_gnutls_con_close;
	srv_sock->release_cb = mod_gnutls_sock_release;
	srv_sock->update_events_cb = mod_gnutls_update_events;
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

	if ((NULL != ca_file) && GNUTLS_E_SUCCESS != (r = gnutls_certificate_set_x509_trust_file(ctx->server_cert, ca_file, GNUTLS_X509_FMT_PEM))) {
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
			if (GNUTLS_E_SUCCESS != (r = gnutls_priority_init(&prio, priority, &errpos))) {
				ERROR(srv, "gnutls_priority_init failed(priority '%s', error at '%s') (%s): %s",
					priority, errpos,
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

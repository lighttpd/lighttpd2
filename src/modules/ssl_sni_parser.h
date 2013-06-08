#ifndef _LIGHTTPD_SSL_SNI_PARSER_H_
#define _LIGHTTPD_SSL_SNI_PARSER_H_

#include <idna.h>

typedef void (*liSSLSniCB)(gpointer data, GString *server_name);

typedef enum {
	LI_SSL_SNI_NOT_FOUND, /* or some error */
	LI_SSL_SNI_FOUND,
	LI_SSL_SNI_WAIT /* need more data */
} liSSLSniParserResult;

typedef struct liSSLSniParser liSSLSniParser;
struct liSSLSniParser {
	guint finished:1, sni_ready:1;

	liChunkParserCtx ctx;

	guint8 record_state; /* how many record header bytes we have (0-4) */
	guint8 record_type;
	guint8 record_protocol_major, record_protocol_minor;
	guint16 record_remaining_length;

	guint8 handshake_state; /* how many handshake header bytes we have (0-3) */
	guint8 handshake_type;
	guint32 handshake_remaining_length;

	guint32 client_hello_state; /* some states (0-12) */
	guint8 client_hello_protocol_major, client_hello_protocol_minor;
	guint16 client_hello_remaining; /* dynamic length stuff */

	guint16 extension_state; /* how many extension header bytes we have (0-4) */
	guint16 extension_type;
	guint16 extension_remaining;

	guint8 sni_state;
	guint8 sni_type;
	guint16 sni_hostname_remaining;
};

typedef struct liSSLSniParserStream liSSLSniParserStream;
struct liSSLSniParserStream {
	liStream stream;
	liSSLSniCB callback;
	gpointer data;
	liSSLSniParser parser;
	GString *server_name;
};

INLINE liSSLSniParserResult li_ssl_sni_parse(liSSLSniParser *context, GString *server_name);


INLINE liStream* li_ssn_sni_stream(liEventLoop *loop, liSSLSniCB callback, gpointer data);
INLINE void li_ssn_sni_stream_ready(liStream *stream); /* key loaded, ready to forward data */


INLINE void li_ssn_sni_stream_cb(liStream *stream, liStreamEvent event); /* private */


/* inline implementations */

INLINE liStream* li_ssn_sni_stream(liEventLoop *loop, liSSLSniCB callback, gpointer data) {
	liSSLSniParserStream *pstream = g_slice_new0(liSSLSniParserStream);
	pstream->server_name = g_string_sized_new(0);
	pstream->callback = callback;
	pstream->data = data;
	li_stream_init(&pstream->stream, loop, li_ssn_sni_stream_cb);
	return &pstream->stream;
}

INLINE void li_ssn_sni_stream_ready(liStream *stream) {
	liSSLSniParserStream *pstream = LI_CONTAINER_OF(stream, liSSLSniParserStream, stream);
	assert(li_ssn_sni_stream_cb == stream->cb);

	pstream->parser.finished = pstream->parser.sni_ready = TRUE;
	li_stream_again(stream);
}

INLINE void li_ssn_sni_stream_cb(liStream *stream, liStreamEvent event) {
	liSSLSniParserStream *pstream = LI_CONTAINER_OF(stream, liSSLSniParserStream, stream);

	switch (event) {
	case LI_STREAM_NEW_DATA:
		if (NULL == stream->source) return;
		if (!pstream->parser.finished) {
			switch (li_ssl_sni_parse(&pstream->parser, pstream->server_name)) {
			case LI_SSL_SNI_NOT_FOUND:
				pstream->parser.sni_ready = TRUE;
				break;
			case LI_SSL_SNI_FOUND:
				pstream->callback(pstream->data, pstream->server_name);
				break;
			case LI_SSL_SNI_WAIT:
				return;
			}
		}
		if (pstream->parser.sni_ready) {
			li_chunkqueue_steal_all(stream->out, stream->source->out);
			if (stream->source->out->is_closed) stream->out->is_closed = TRUE;
			li_stream_notify(stream);
		}
		break;
	case LI_STREAM_NEW_CQLIMIT:
		break;
	case LI_STREAM_CONNECTED_DEST:
		break;
	case LI_STREAM_CONNECTED_SOURCE:
		li_chunk_parser_init(&pstream->parser.ctx, stream->source->out);
		break;
	case LI_STREAM_DISCONNECTED_DEST:
		pstream->parser.finished = pstream->parser.sni_ready = TRUE;
		li_stream_disconnect(stream);
		break;
	case LI_STREAM_DISCONNECTED_SOURCE:
		pstream->parser.finished = pstream->parser.sni_ready = TRUE;
		li_stream_disconnect_dest(stream);
		break;
	case LI_STREAM_DESTROY:
		pstream->callback = NULL;
		pstream->data = NULL;
		g_string_free(pstream->server_name, TRUE);
		pstream->server_name = NULL;
		g_slice_free(liSSLSniParserStream, pstream);
		break;
	}
}

INLINE liSSLSniParserResult li_ssl_sni_parse(liSSLSniParser *context, GString *server_name) {
	guint8 *p = NULL, *global_pe = NULL;

	if (context->finished) goto fail;

	li_chunk_parser_prepare(&context->ctx);

	for (;;) {
		guint8 *record_pe;

		while (NULL == p || p >= global_pe) {
			liHandlerResult res = li_chunk_parser_next(&context->ctx, (gchar**) &p, (gchar**) &global_pe, NULL);
			if (LI_HANDLER_WAIT_FOR_EVENT == res && !context->ctx.cq->is_closed) return context->finished ? LI_SSL_SNI_NOT_FOUND : LI_SSL_SNI_WAIT;
			if (LI_HANDLER_GO_ON != res) goto fail;
		}

		switch (context->record_state) {
		case 0:
			if (context->record_remaining_length > 0) break;
			li_chunk_parser_done(&context->ctx, 1);
			context->record_type = *p++;
			++context->record_state;
			if (22 != context->record_type) goto fail; /* abort if not handshake */
			continue;
		case 1:
			li_chunk_parser_done(&context->ctx, 1);
			context->record_protocol_major = *p++;
			++context->record_state;
			continue;
		case 2:
			li_chunk_parser_done(&context->ctx, 1);
			context->record_protocol_minor = *p++;
			++context->record_state;
			continue;
		case 3:
			li_chunk_parser_done(&context->ctx, 1);
			context->record_remaining_length = (*p++) << 8;
			++context->record_state;
			continue;
		case 4:
			li_chunk_parser_done(&context->ctx, 1);
			context->record_remaining_length += (*p++);
			context->record_state = 0;
			if (context->record_remaining_length > ((1u << 14) + 2048u)) goto fail;
			continue;
		}

		{
			/* we will parse all available bytes */
			guint take = MIN(context->record_remaining_length, (guint) (global_pe - p));
			context->record_remaining_length -= take;
			li_chunk_parser_done(&context->ctx, take);
			record_pe = p + take;
		}

		while (p < record_pe) {
			guint8 *client_hello_pe;

			/* we only parse handshake records */
			switch (context->handshake_state) {
			case 0:
				if (context->handshake_remaining_length > 0) break;
				context->handshake_type = *p++;
				++context->handshake_state;
				if (1 != context->handshake_type) goto fail; /* abort if not client_hello */
				continue;
			case 1:
				context->handshake_remaining_length = (*p++) << 16;
				++context->handshake_state;
				continue;
			case 2:
				context->handshake_remaining_length += (*p++) << 8;
				++context->handshake_state;
				continue;
			case 3:
				context->handshake_remaining_length += (*p++);
				context->handshake_state = 0;
				context->client_hello_state = 0;
				continue;
			}

			/* we only parse client_hello handshakes */
			{
				guint take = MIN(context->handshake_remaining_length, (guint) (record_pe - p));
				context->handshake_remaining_length -= take;
				client_hello_pe = p + take;
				if (0 == context->handshake_remaining_length) context->finished = TRUE; /* after this there is no point searching */
			}

			while (p < client_hello_pe) {
				switch (context->client_hello_state) {
				case 2: /* skip random */
				case 4: /* skip session id */
				case 7: /* skip ciphers */
				case 9: /* skip compression methods */
					{
						guint take = MIN(context->client_hello_remaining, (guint) (client_hello_pe - p));
						context->client_hello_remaining -= take;
						p += take;
					}

					if (0 == context->client_hello_remaining) {
						++context->client_hello_state;
					}
					continue;
				case 0: /* protocol major */
					context->client_hello_protocol_major = (*p++);
					++context->client_hello_state;
					continue;
				case 1: /* protocol minor */
					context->client_hello_protocol_minor = (*p++);
					++context->client_hello_state;
					context->client_hello_remaining = 32; /* length of "random" data is constant */
					continue;
				case 3: /* session id length */
					context->client_hello_remaining = (*p++);
					++context->client_hello_state;
					continue;
				case 5: /* cipher length upper byte */
					context->client_hello_remaining = (*p++) << 16;
					++context->client_hello_state;
					continue;
				case 6: /* cipher length lower byte */
					context->client_hello_remaining += (*p++);
					++context->client_hello_state;
					if (0 != context->client_hello_remaining % 2) goto fail;
					continue;
				case 8: /* compression methods length */
					context->client_hello_remaining = (*p++);
					++context->client_hello_state;
					continue;
				case 10: /* extensions length upper byte */
					context->client_hello_remaining = (*p++) << 16;
					++context->client_hello_state;
					continue;
				case 11: /* extensions length lower byte */
					context->client_hello_remaining += (*p++);
					++context->client_hello_state;

					/* extensions fill the record exactly if present */
					if (context->handshake_remaining_length + (guint) (client_hello_pe - p) != context->client_hello_remaining) goto fail;
					context->extension_state = 0;
					continue;
				case 12: /* extensions data */
					break;
				}

				/* parse extensions */
				{
					guint take = MIN(context->client_hello_remaining, (guint) (client_hello_pe - p));
					context->client_hello_remaining -= take;
					assert(client_hello_pe == p + take);
					if (0 == context->client_hello_remaining) context->finished = TRUE; /* after this there is no point searching */
				}

				while (p < client_hello_pe) {
					guint8 *extension_pe;

					switch (context->extension_state) {
					case 0:
						context->extension_type = (*p++) << 8;
						++context->extension_state;
						continue;
					case 1:
						context->extension_type += (*p++);
						++context->extension_state;
						continue;
					case 2:
						context->extension_remaining = (*p++) << 8;
						++context->extension_state;
						continue;
					case 3:
						context->extension_remaining += (*p++);
						++context->extension_state;
						continue;
					case 4:
						if (0 == context->extension_type) break; /* parse this one: SNI */
						{
							/* ignore extension */
							guint take = MIN(context->extension_remaining, (guint) (client_hello_pe - p));
							context->extension_remaining -= take;
							p += take;
							if (0 == context->extension_remaining) context->extension_state = 0;
						}
						continue;
					}

					{
						guint take = MIN(context->extension_remaining, (guint) (client_hello_pe - p));
						context->extension_remaining -= take;
						extension_pe = p + take;
						if (0 == context->extension_remaining) context->finished = TRUE; /* after this there is no point searching */
					}

					while (p < extension_pe) {
						/* SNI extension. append data to string */
						switch (context->sni_state) {
						case 0: /* ignore list length */
						case 1: /* ignore list length */
							p++;
							++context->sni_state;
							continue;
						case 2: /* type of first entry */
							context->sni_type = (*p++);
							if (0 != context->sni_type) goto fail; /* no spec how to parse unknown entries, can't skip them as we don't know their length */
							++context->sni_state;
							continue;
						case 3:
							context->sni_hostname_remaining = (*p++) << 8;
							++context->sni_state;
							continue;
						case 4:
							context->sni_hostname_remaining += (*p++);
							++context->sni_state;
							continue;
						case 5:
							{
								guint take = MIN(context->sni_hostname_remaining, (guint) (extension_pe - p));
								g_string_append_len(server_name, (gchar*) p, take);
								context->sni_hostname_remaining -= take;
								p += take;
								if (0 == context->sni_hostname_remaining) goto found_sni;
							}
						}
					}
				} /* while (p >= client_hello_pe) -- extension subloop */
			} /* while (p >= client_hello_pe) */

			if (0 == context->handshake_remaining_length) goto fail; /* client_hello ended, no sni found */
		} /* while (p >= record_pe) */

	}

found_sni:
	{
		char *ascii_sni;
		if (IDNA_SUCCESS == idna_to_ascii_8z(server_name->str, &ascii_sni, IDNA_ALLOW_UNASSIGNED)) {
			g_string_assign(server_name, ascii_sni);
			free(ascii_sni);
		} else {
			goto fail;
		}
	}
	context->finished = TRUE;
	return LI_SSL_SNI_FOUND;

fail:
	context->finished = TRUE;
	return LI_SSL_SNI_NOT_FOUND;
}

#endif

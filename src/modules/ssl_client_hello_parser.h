#ifndef _LIGHTTPD_SSL_SNI_PARSER_H_
#define _LIGHTTPD_SSL_SNI_PARSER_H_

#ifdef USE_SNI
#include <idna.h>
#endif

typedef void (*liSSLClientHelloCB)(gpointer data, gboolean success, GString *server_name, guint16 client_hello_protocol);

typedef enum {
	LI_SSL_CLIENT_HELLO_ERROR,
	LI_SSL_CLIENT_HELLO_FOUND,
	LI_SSL_CLIENT_HELLO_WAIT /* need more data */
} liSSLClientHelloParserResult;

typedef struct liSSLClientHelloParser liSSLClientHelloParser;
struct liSSLClientHelloParser {
	guint finished:1, forward:1, sni_complete:1;

	liChunkParserCtx ctx;

	guint8 record_state; /* how many record header bytes we have (0-4) */
	guint8 record_type;
	guint16 record_protocol;
	guint16 record_remaining_length;

	guint8 handshake_state; /* how many handshake header bytes we have (0-3) */
	guint8 handshake_type;
	guint32 handshake_remaining_length;

	guint32 client_hello_state; /* some states (0-12) */
	guint16 client_hello_protocol;
	guint16 client_hello_remaining; /* dynamic length stuff */

	guint16 extension_state; /* how many extension header bytes we have (0-4) */
	guint16 extension_type;
	guint16 extension_remaining;

	guint8 sni_state;
	guint8 sni_type;
	guint16 sni_hostname_remaining;
};

typedef struct liSSLClientHelloParserStream liSSLClientHelloParserStream;
struct liSSLClientHelloParserStream {
	liStream stream;
	liSSLClientHelloCB callback;
	gpointer data;
	liSSLClientHelloParser parser;
	GString *server_name;
};

INLINE liStream* li_ssl_client_hello_stream(liEventLoop *loop, liSSLClientHelloCB callback, gpointer data);
INLINE void li_ssl_client_hello_stream_ready(liStream *stream); /* key loaded, ready to forward data */


/* private */
INLINE liSSLClientHelloParserResult _li_ssl_client_hello_parse(liSSLClientHelloParser *context, GString *server_name);
INLINE void _li_ssl_client_hello_stream_cb(liStream *stream, liStreamEvent event);


/* inline implementations */

INLINE liStream* li_ssl_client_hello_stream(liEventLoop *loop, liSSLClientHelloCB callback, gpointer data) {
	liSSLClientHelloParserStream *pstream = g_slice_new0(liSSLClientHelloParserStream);
	pstream->server_name = g_string_sized_new(0);
	pstream->callback = callback;
	pstream->data = data;
	li_stream_init(&pstream->stream, loop, _li_ssl_client_hello_stream_cb);
	return &pstream->stream;
}

INLINE void li_ssl_client_hello_stream_ready(liStream *stream) {
	liSSLClientHelloParserStream *pstream = LI_CONTAINER_OF(stream, liSSLClientHelloParserStream, stream);
	assert(_li_ssl_client_hello_stream_cb == stream->cb);

	pstream->parser.finished = pstream->parser.forward = TRUE;
	li_stream_again(stream);
}

INLINE void _li_ssl_client_hello_stream_cb(liStream *stream, liStreamEvent event) {
	liSSLClientHelloParserStream *pstream = LI_CONTAINER_OF(stream, liSSLClientHelloParserStream, stream);

	switch (event) {
	case LI_STREAM_NEW_DATA:
		if (NULL == stream->source) return;
		if (!pstream->parser.finished) {
			switch (_li_ssl_client_hello_parse(&pstream->parser, pstream->server_name)) {
			case LI_SSL_CLIENT_HELLO_ERROR:
				pstream->callback(pstream->data, FALSE, NULL, 0);
				break;
			case LI_SSL_CLIENT_HELLO_FOUND:
				pstream->callback(pstream->data, TRUE, pstream->server_name, pstream->parser.client_hello_protocol);
				break;
			case LI_SSL_CLIENT_HELLO_WAIT:
				return;
			}
		}
		if (pstream->parser.forward) {
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
		pstream->parser.finished = TRUE;
		li_stream_disconnect(stream);
		break;
	case LI_STREAM_DISCONNECTED_SOURCE:
		pstream->parser.finished = TRUE;
		li_stream_disconnect_dest(stream);
		break;
	case LI_STREAM_DESTROY:
		pstream->callback = NULL;
		pstream->data = NULL;
		g_string_free(pstream->server_name, TRUE);
		pstream->server_name = NULL;
		g_slice_free(liSSLClientHelloParserStream, pstream);
		break;
	}
}

#define CLIENT_HELLO_PARSER_DEBUG 0
#if CLIENT_HELLO_PARSER_DEBUG
# define PARSER_FAIL(...) do { fprintf(stderr, "ssl_client_hello_parse fail: " __VA_ARGS__); goto fail; } while (0)
#else
# define PARSER_FAIL(...) do { goto fail; } while (0)
#endif

INLINE liSSLClientHelloParserResult _li_ssl_client_hello_parse(liSSLClientHelloParser *context, GString *server_name) {
	guint8 *p = NULL, *global_pe = NULL;

	if (context->finished) return LI_SSL_CLIENT_HELLO_ERROR;

	li_chunk_parser_prepare(&context->ctx);

	for (;;) {
		guint8 *record_pe;

		while (NULL == p || p >= global_pe) {
			liHandlerResult res = li_chunk_parser_next(&context->ctx, (gchar**) &p, (gchar**) &global_pe, NULL);
			if (LI_HANDLER_WAIT_FOR_EVENT == res && !context->ctx.cq->is_closed) return context->finished ? LI_SSL_CLIENT_HELLO_ERROR : LI_SSL_CLIENT_HELLO_WAIT;
			if (LI_HANDLER_GO_ON != res) goto fail;
		}

		switch (context->record_state) {
		case 0:
			if (context->record_remaining_length > 0) break;
			li_chunk_parser_done(&context->ctx, 1);
			context->record_type = *p++;
			++context->record_state;
			if (22 != context->record_type) PARSER_FAIL("not a handshake record: %i\n", context->record_type);
			continue;
		case 1:
			li_chunk_parser_done(&context->ctx, 1);
			context->record_protocol = (*p++) << 8;
			++context->record_state;
			continue;
		case 2:
			li_chunk_parser_done(&context->ctx, 1);
			context->record_protocol |= *p++;
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
			if (context->record_remaining_length > ((1u << 14) + 2048u)) PARSER_FAIL("record too long\n");
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
				context->handshake_type = *p++;
				++context->handshake_state;
				if (1 != context->handshake_type) PARSER_FAIL("handshake isn't client_hello\n");
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
				context->handshake_state = 4;
				context->client_hello_state = 0;
				continue;
			case 4:
				if (context->handshake_remaining_length > 0) break;
				goto client_hello_finished;
			}

			/* we only parse client_hello handshakes */
			{
				guint take = MIN(context->handshake_remaining_length, (guint) (record_pe - p));
				context->handshake_remaining_length -= take;
				client_hello_pe = p + take;
			}

			while (p < client_hello_pe) {
#if CLIENT_HELLO_PARSER_DEBUG
				fprintf(stderr, "ssl_sni_parse client_hello: state %i, remaining: %i\n", context->client_hello_state, context->client_hello_remaining);
#endif
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
					context->client_hello_protocol = (*p++) << 8;
					++context->client_hello_state;
					continue;
				case 1: /* protocol minor */
					context->client_hello_protocol |= (*p++);
					++context->client_hello_state;
					context->client_hello_remaining = 32; /* length of "random" data is constant */
					continue;
				case 3: /* session id length */
					context->client_hello_remaining = (*p++);
					++context->client_hello_state;
					continue;
				case 5: /* cipher length upper byte */
					context->client_hello_remaining = (*p++) << 8;
					++context->client_hello_state;
					continue;
				case 6: /* cipher length lower byte */
					context->client_hello_remaining += (*p++);
					++context->client_hello_state;
					if (0 != context->client_hello_remaining % 2) PARSER_FAIL("client_hello cipher length is odd\n");
					continue;
				case 8: /* compression methods length */
					context->client_hello_remaining = (*p++);
					++context->client_hello_state;
					continue;
				case 10: /* extensions length upper byte */
					context->client_hello_remaining = (*p++) << 8;
					++context->client_hello_state;
					continue;
				case 11: /* extensions length lower byte */
					context->client_hello_remaining += (*p++);
					++context->client_hello_state;

#if CLIENT_HELLO_PARSER_DEBUG
					fprintf(stderr, "ssl_sni_parse client_hello: extensions length %i, can read %i\n", context->client_hello_remaining, (guint) (client_hello_pe - p));
#endif
					/* extensions fill the record exactly if present */
					if (context->handshake_remaining_length + (guint) (client_hello_pe - p) != context->client_hello_remaining)
						PARSER_FAIL("client_hello extensions don't fill the handshake\n");
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
						if (0 == context->extension_remaining) {
							context->extension_state = 0;
							continue;
						}
#if CLIENT_HELLO_PARSER_DEBUG
						fprintf(stderr, "ssl_sni_parse: extension type %i\n", context->extension_type);
#endif
#ifdef USE_SNI
						if (0 == context->extension_type) break; /* parse this one: SNI */
#endif
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
							if (0 == context->sni_type && !context->sni_complete) {
								guint take = MIN(context->sni_hostname_remaining, (guint) (extension_pe - p));
								g_string_append_len(server_name, (gchar*) p, take);
								context->sni_hostname_remaining -= take;
								p += take;
								if (0 == context->sni_hostname_remaining) {
#ifdef USE_SNI
									char *ascii_sni;
									if (IDNA_SUCCESS == idna_to_ascii_8z(server_name->str, &ascii_sni, IDNA_ALLOW_UNASSIGNED)) {
										context->sni_complete = TRUE; /* pick the first decodable hostname */
										g_string_assign(server_name, ascii_sni);
										free(ascii_sni);
									} else {
										g_string_truncate(server_name, 0); /* ignore */
									}
#else
									g_string_truncate(server_name, 0); /* ignore */
#endif
								}
							} else {
								guint take = MIN(context->sni_hostname_remaining, (guint) (extension_pe - p));
								context->sni_hostname_remaining -= take;
								p += take;
							}
						}
					} /* while (p < extension_pe) -- sni extension subloop */
				} /* while (p < client_hello_pe) -- extensions subloop */
			} /* while (p < client_hello_pe) */
			if (0 == context->handshake_remaining_length) goto client_hello_finished;
		} /* while (p < record_pe) */
	}

client_hello_finished:
#if CLIENT_HELLO_PARSER_DEBUG
	fprintf(stderr, "ssl_sni_parse: parsing client_hello done\n");
#endif
	context->finished = TRUE;
	return LI_SSL_CLIENT_HELLO_FOUND;

fail:
	context->finished = TRUE;
	return LI_SSL_CLIENT_HELLO_ERROR;
}
#undef CLIENT_HELLO_PARSER_DEBUG
#undef PARSER_FAIL

#endif

/* PROXY protocol filter:
 * https://github.com/haproxy/haproxy/blob/master/doc/proxy-protocol.txt
 */

#include <lighttpd/base.h>
#include <lighttpd/plugin_core.h>

void li_proxy_protocol_data_init(liProxyProtocolData *data) {
	memset(data, 0, sizeof(*data));
}

void li_proxy_protocol_data_clear(liProxyProtocolData *data) {
	li_sockaddr_clear(&data->remote);
	li_sockaddr_clear(&data->local);
	if (data->tlvs) g_byte_array_free(data->tlvs, TRUE);
	memset(data, 0, sizeof(*data));
}

#define PROXY_V1_SIG "PROXY "
#define PROXY_V2_SIG "\x0D\x0A\x0D\x0A\x00\x0D\x0AQUIT\x0A"
#define PROXY_V1_SIG_LENGTH ((gssize)(sizeof(PROXY_V1_SIG) - 1))
#define PROXY_V2_SIG_LENGTH ((gssize)(sizeof(PROXY_V2_SIG) - 1))
#define PROXY_V2_HEADER_LENGTH 16
#define PROXY_V1_MAX_LENGTH 108
#define PROXY_V2_MAX_INITIAL_LENGTH (PROXY_V2_HEADER_LENGTH + 2 * 108)  /* not counting TLVs */

static liSocketAddress raw_ipv4_addr(unsigned char *raw_addr, unsigned char *raw_port) {
	liSocketAddress saddr = { 0, { NULL } };
	saddr.len = sizeof(struct sockaddr_in);
	saddr.addr_up.ipv4 = (struct sockaddr_in*)g_slice_alloc0(saddr.len);
	saddr.addr_up.ipv4->sin_family = AF_INET;
	memcpy(&saddr.addr_up.ipv4->sin_addr, raw_addr, sizeof(saddr.addr_up.ipv4->sin_addr));
	memcpy(&saddr.addr_up.ipv4->sin_port, raw_port, sizeof(saddr.addr_up.ipv4->sin_port));
	return saddr;
}

static liSocketAddress raw_ipv6_addr(unsigned char *raw_addr, unsigned char *raw_port) {
	liSocketAddress saddr = { 0, { NULL } };
	saddr.len = sizeof(struct sockaddr_in6);
	saddr.addr_up.ipv6 = (struct sockaddr_in6*)g_slice_alloc0(saddr.len);
	saddr.addr_up.ipv6->sin6_family = AF_INET6;
	memcpy(&saddr.addr_up.ipv6->sin6_addr, raw_addr, sizeof(saddr.addr_up.ipv6->sin6_addr));
	memcpy(&saddr.addr_up.ipv6->sin6_port, raw_port, sizeof(saddr.addr_up.ipv6->sin6_port));
	return saddr;
}

static liSocketAddress raw_unix_addr(unsigned char *raw_addr) {
	liSocketAddress saddr = { 0, { NULL } };
	saddr.len = sizeof(struct sockaddr_un);
	saddr.addr_up.un = (struct sockaddr_un*)g_slice_alloc0(saddr.len);
	saddr.addr_up.un->sun_family = AF_UNIX;
	memcpy(saddr.addr_up.un->sun_path, raw_addr, 108);
	return saddr;
}

static liProxyProtocolParseResult proxy_prot_parse_v2(
	liVRequest *vr,
	liProxyProtocolData *data,
	unsigned char *header,
	gssize header_len
) {
	unsigned char version = header[12] >> 4;
	unsigned char command = header[12] & 0xf;
	unsigned char family = header[13] >> 4;
	unsigned char raw_transport = header[13] & 0xf;
	gssize payload_len = ((guint)header[14]) << 8 | header[15];
	gssize total_len = payload_len + PROXY_V2_HEADER_LENGTH;
	gssize available_payload_len = header_len - PROXY_V2_HEADER_LENGTH;
	unsigned char* address_data = header + PROXY_V2_HEADER_LENGTH;
	gssize max_tlv_length = CORE_OPTION(LI_CORE_OPTION_PROXY_PROTOCOL_TLV_MAX_LENGTH).number;
	gssize required_address_len;

	if (version != 2) {
		if (CORE_OPTION(LI_CORE_OPTION_DEBUG_REQUEST_HANDLING).boolean) {
			VR_DEBUG(vr, "Invalid PROXY protocol version in binary header: %i", version);
		}
		return LI_PROXY_PROTOCOL_PARSE_ERROR;
	}
	if (command == 0) {
		/* LOCAL: skip completely */
		data->skip_bytes = total_len;
		return 0;
	} else if (command != 1) {
		if (CORE_OPTION(LI_CORE_OPTION_DEBUG_REQUEST_HANDLING).boolean) {
			VR_DEBUG(vr, "Invalid PROXY protocol command: %u", command);
		}
		return LI_PROXY_PROTOCOL_PARSE_ERROR;
	}
	if (raw_transport > LI_PROXY_PROT_TRANSPORT_MAX) {
		if (CORE_OPTION(LI_CORE_OPTION_DEBUG_REQUEST_HANDLING).boolean) {
			VR_DEBUG(vr, "Invalid PROXY protocol transport: %u", raw_transport);
		}
		return LI_PROXY_PROTOCOL_PARSE_ERROR;
	}

	switch (family) {
	case 0x00: /* AF_UNSPEC: skip completely */
		required_address_len = 0;
		break;
	case 0x01: /* AF_INET, IPv4 */
		required_address_len = 12;
		break;
	case 0x02: /* AF_INET6, IPv6 */
		required_address_len = 36;
		break;
	case 0x03: /* AF_UNIX */
		required_address_len = 216;
		break;
	default:
		if (CORE_OPTION(LI_CORE_OPTION_DEBUG_REQUEST_HANDLING).boolean) {
			VR_DEBUG(vr, "Invalid PROXY protocol family: %u", family);
		}
		return LI_PROXY_PROTOCOL_PARSE_ERROR;
	}

	if (required_address_len > payload_len) {
		if (CORE_OPTION(LI_CORE_OPTION_DEBUG_REQUEST_HANDLING).boolean) {
			VR_DEBUG(vr, "Invalid PROXY protocol address length; require %u, got %u", required_address_len, payload_len);
		}
		return LI_PROXY_PROTOCOL_PARSE_ERROR;
	}
	if (required_address_len > available_payload_len) return LI_PROXY_PROTOCOL_PARSE_NEED_MORE_DATA;

	switch (family) {
	case 0x01: /* AF_INET, IPv4 */
		data->remote = raw_ipv4_addr(address_data, address_data + 2*4);
		data->local = raw_ipv4_addr(address_data + 4, address_data + 2*4 + 2);
		break;
	case 0x02: /* AF_INET6, IPv6 */
		data->remote = raw_ipv6_addr(address_data, address_data + 2*16);
		data->local = raw_ipv6_addr(address_data + 16, address_data + 2*16 + 2);
		break;
	case 0x03: /* AF_UNIX */
		data->remote = raw_unix_addr(address_data);
		data->local = raw_unix_addr(address_data + 108);
		break;
	}

	data->version = version;
	data->transport = (liProxyProtTransport) raw_transport;

	/* spec says for AF_UNSPEC: "The receiver should ignore address information." (it doesn't say the address has zero length!)*/
	if (0 == required_address_len || max_tlv_length < 0) {
		data->skip_bytes = total_len;
	} else {
		data->skip_bytes = required_address_len + PROXY_V2_HEADER_LENGTH;
		data->remaining_tlv_bytes = total_len - data->skip_bytes; /* should equal `payload_len - required_address_len` */

		if (data->remaining_tlv_bytes > max_tlv_length) {
			if (CORE_OPTION(LI_CORE_OPTION_DEBUG_REQUEST_HANDLING).boolean) {
				VR_DEBUG(vr, "PROXY TLV section too big: %u > %u", data->remaining_tlv_bytes, data->remaining_tlv_bytes);
			}
			return LI_PROXY_PROTOCOL_PARSE_ERROR;
		}
	}

	return LI_PROXY_PROTOCOL_PARSE_DONE;
}

static gboolean str_to_port(unsigned char *str, guint16 *port) {
	gchar *end = NULL;
	guint64 port64;

	if (str[0] == '0' && str[1] != '\0') return FALSE;
	if (str[0] == '-') return FALSE;

	port64 = g_ascii_strtoull(str, &end, 10);
	if (port64 == 0 && end == (gchar*)str) return FALSE;
	if (*end != '\0') return FALSE;
	if (port64 > 0xffff) return FALSE;

	*port = port64;
	return TRUE;
}

static gboolean parse_ipv4_port(unsigned char *s_ip, unsigned char *s_port, liSocketAddress *saddr) {
	guint32 ipv4;
	guint16 port = 0;

	if (!str_to_port(s_port, &port)) return FALSE;
	if (!li_parse_ipv4(s_ip, &ipv4, NULL, NULL)) return FALSE;

	saddr->len = sizeof(struct sockaddr_in);
	saddr->addr_up.ipv4 = (struct sockaddr_in*)g_slice_alloc0(saddr->len);
	saddr->addr_up.ipv4->sin_family = AF_INET;
	saddr->addr_up.ipv4->sin_addr.s_addr = ipv4;
	saddr->addr_up.ipv4->sin_port = htons(port);

	return TRUE;
}

static gboolean parse_ipv6_port(unsigned char *s_ip, unsigned char *s_port, liSocketAddress *saddr) {
	guint8 ipv6[16];
	guint16 port = 0;

	if (!str_to_port(s_port, &port)) return FALSE;
	if (!li_parse_ipv6(s_ip, ipv6, NULL, NULL)) return FALSE;

	saddr->len = sizeof(struct sockaddr_in6);
	saddr->addr_up.ipv6 = (struct sockaddr_in6*)g_slice_alloc0(saddr->len);
	saddr->addr_up.ipv6->sin6_family = AF_INET6;
	memcpy(&saddr->addr_up.ipv6->sin6_addr, ipv6, 16);
	saddr->addr_up.ipv6->sin6_port = htons(port);

	return TRUE;
}

static liProxyProtocolParseResult proxy_prot_parse_v1(
	liVRequest *vr,
	liProxyProtocolData *data,
	unsigned char *header,
	gssize header_len
) {
	unsigned char *cr;
	unsigned char *s_remote_ip;
	unsigned char *s_local_ip;
	unsigned char *s_remote_port;
	unsigned char *s_local_port;
	gboolean ipv4;

	UNUSED(data);

	if (NULL == (cr = memchr(header, '\r', header_len - 1))) {
		if (CORE_OPTION(LI_CORE_OPTION_DEBUG_REQUEST_HANDLING).boolean) {
			VR_DEBUG(vr, "%s", "PROXY protocol v1: header incomplete");
		}
		return LI_PROXY_PROTOCOL_PARSE_ERROR;
	}
	if (cr[1] != '\n') {
		if (CORE_OPTION(LI_CORE_OPTION_DEBUG_REQUEST_HANDLING).boolean) {
			VR_DEBUG(vr, "%s", "PROXY protocol v1: invalid header end");
		}
		return LI_PROXY_PROTOCOL_PARSE_ERROR;
	}
	data->skip_bytes = cr + 2 - header;
	cr[0] = '\0';

	header += PROXY_V1_SIG_LENGTH;
	if (g_str_has_prefix(header, "TCP4 ")) {
		ipv4 = TRUE;
	} else if (g_str_has_prefix(header, "TCP6 ")) {
		ipv4 = FALSE;
	} else if (g_str_has_prefix(header, "UNKNOWN ")) {
		data->version = 1;
		return LI_PROXY_PROTOCOL_PARSE_DONE;
	} else goto invalid;

	s_remote_ip = header + 5; /* skip "TCP4 " or "TCP6 " to get source address */
	header = strchr(s_remote_ip, ' ');
	if (NULL == header) goto invalid;
	*header = '\0';
	s_local_ip = header + 1; /* destination address, skip space */
	header = strchr(s_local_ip, ' ');
	if (NULL == header) goto invalid;
	*header = '\0';
	s_remote_port = header + 1;  /* skip space */
	header = strchr(s_remote_port, ' ');
	if (NULL == header) goto invalid;
	*header = '\0';
	s_local_port = header + 1;  /* skip space */

	if (ipv4) {
		if (!parse_ipv4_port(s_remote_ip, s_remote_port, &data->remote)) goto invalid;
		if (!parse_ipv4_port(s_local_ip, s_local_port, &data->local)) goto invalid;
	} else {
		if (!parse_ipv6_port(s_remote_ip, s_remote_port, &data->remote)) goto invalid;
		if (!parse_ipv6_port(s_local_ip, s_local_port, &data->local)) goto invalid;
	}

	data->version = 1;

	return LI_PROXY_PROTOCOL_PARSE_DONE;

invalid:
	if (CORE_OPTION(LI_CORE_OPTION_DEBUG_REQUEST_HANDLING).boolean) {
		VR_DEBUG(vr, "%s", "PROXY protocol v1: invalid header");
	}
	return LI_PROXY_PROTOCOL_PARSE_ERROR;
}

liProxyProtocolParseResult li_proxy_protocol_parse(
	liVRequest *vr,
	liProxyProtocolData *data,
	unsigned char *header,
	gssize header_len
) {
	if (0 == header_len) return LI_PROXY_PROTOCOL_PARSE_NEED_MORE_DATA;

	if (0 == memcmp(header, PROXY_V2_SIG, MIN(PROXY_V2_SIG_LENGTH, header_len))) {
		if (header_len < PROXY_V2_HEADER_LENGTH) return LI_PROXY_PROTOCOL_PARSE_NEED_MORE_DATA;
		return proxy_prot_parse_v2(vr, data, header, header_len);
	}

	if (0 == memcmp(header, PROXY_V1_SIG, MIN(PROXY_V1_SIG_LENGTH, header_len))) {
		if (header_len < PROXY_V1_SIG_LENGTH) return LI_PROXY_PROTOCOL_PARSE_NEED_MORE_DATA;
		return proxy_prot_parse_v1(vr, data, header, header_len);
	}

	return LI_PROXY_PROTOCOL_PARSE_DONE;
}

static gboolean proxy_prot_parse_cq(liConnectionProxyProtocolFilter *filter, liChunkQueue *in, liProxyProtocolData *data) {
	liChunkIter ci = li_chunkqueue_iter(in);
	unsigned char *header = NULL;
	off_t header_len = 0;
	ssize_t parsed_len;
	liConnection *con = LI_CONTAINER_OF(filter, liConnection, proxy_protocol_filter);
	liVRequest *vr = con->mainvr;

	/* protocol spec says we should expect all data in one read (no reason it
	 * would get segmented on the way, and must be sent in one go)
	 *
	 * But the TLV extensions could get quite big, so we only expect
	 * the address data as part of the initial header.
	 */

	if (in->length == 0) return TRUE; /* no data yet */

	if (data->version == 0) {
		/* initial header not parsed yet */
		{
			GError *err = NULL;
			if (LI_HANDLER_GO_ON != li_chunkiter_read(ci, 0, PROXY_V2_MAX_INITIAL_LENGTH, (char **) &header, &header_len, &err)) {
				VR_ERROR(vr,
					"failed to read data to parse PROXY protocol: %s",
					err ? err->message : "(unknown)"
				);
				if (err) g_error_free(err);
				return FALSE;
			}
		}

		parsed_len = li_proxy_protocol_parse(vr, data, header, (size_t) header_len);
		switch (parsed_len) {
		case LI_PROXY_PROTOCOL_PARSE_NEED_MORE_DATA:
			/* not enough data ("slow start"), but what we got could be a PROXY header */
			if (CORE_OPTION(LI_CORE_OPTION_DEBUG_REQUEST_HANDLING).boolean) {
				/* we actually only expect the address data for PROXY v2 to be in the initial part */
				VR_DEBUG(vr, "%s", "Segmented handshake starts with partial PROXY header; refuse (must be received as one segment)");
			}
			return FALSE;
		case LI_PROXY_PROTOCOL_PARSE_ERROR:
			/* error already got logged */
			return FALSE;
		case LI_PROXY_PROTOCOL_PARSE_DONE:
			break;
		}
	}

	if (data->skip_bytes) {
		/* first skip initial header / address data */
		data->skip_bytes -= li_chunkqueue_skip(in, data->skip_bytes);
		if (data->skip_bytes) {
			/* need more data to skip */
			return TRUE;
		}
	}

	if (data->remaining_tlv_bytes) {
		if (data->remaining_tlv_bytes > in->length) {
			/* need more data so we can extract TLVs in one step */
			return TRUE;
		}

		if (NULL == data->tlvs) {
			data->tlvs = g_byte_array_sized_new(data->remaining_tlv_bytes);
		}
		{
			GError *err;
			if (!li_chunkqueue_extract_to_bytearr(in, data->remaining_tlv_bytes, data->tlvs, &err)) {
				VR_ERROR(vr,
					"failed to extract TLV data for PROXY protocol: %s",
					err ? err->message : "(unknown)"
				);
				if (err) g_error_free(err);
				return FALSE;
			}
		}
		data->remaining_tlv_bytes = 0;
	}

	filter->done = TRUE;

	return TRUE;
}

static void proxy_prot_handle_data(liConnectionProxyProtocolFilter *filter) {
	liConnection *con = LI_CONTAINER_OF(filter, liConnection, proxy_protocol_filter);
	liChunkQueue *out = filter->stream.out;
	liChunkQueue *in;

	if (out->is_closed) {
		li_stream_disconnect(&filter->stream);
		return;
	}

	in = (filter->stream.source != NULL) ? filter->stream.source->out : NULL;
	if (NULL != in) {
		if (!filter->done) {
			if (!proxy_prot_parse_cq(filter, in, &con->info.proxy_prot_data)) {
				li_stream_reset(&filter->stream);
				return;
			}
		}
		/* might be done now (or not) */
		if (filter->done) {
			if (in->length > 0) {
				li_chunkqueue_steal_all(out, in);
				if (in->is_closed) out->is_closed = TRUE;
				li_stream_notify(&filter->stream);
				return;  /* `in` might be gone now after notify */
			}
		}
	}

	if (NULL == in || in->is_closed) {
		out->is_closed = TRUE;
		li_stream_notify(&filter->stream); /* if no flush happened we still notify */
		li_stream_disconnect(&filter->stream);
	}
}

static void proxy_prot_cb(liStream *stream, liStreamEvent event) {
	liConnectionProxyProtocolFilter *filter = LI_CONTAINER_OF(stream, liConnectionProxyProtocolFilter, stream);

	switch (event) {
	case LI_STREAM_NEW_DATA:
		proxy_prot_handle_data(filter);
		break;
	case LI_STREAM_NEW_CQLIMIT:
		break;
	case LI_STREAM_CONNECTED_DEST:
		break;
	case LI_STREAM_CONNECTED_SOURCE:
		break;
	case LI_STREAM_DISCONNECTED_DEST:
		li_stream_disconnect(stream);
		break;
	case LI_STREAM_DISCONNECTED_SOURCE:
		li_stream_disconnect_dest(stream);
		break;
	case LI_STREAM_DESTROY:
		{
			liConnection *con = LI_CONTAINER_OF(filter, liConnection, proxy_protocol_filter);
			li_job_later(&con->wrk->loop.jobqueue, &con->job_reset);
		}
		break;
	}
}

void li_connection_proxy_protocol_init(liConnection *con) {
	con->info.proxy_prot_used = FALSE;
	con->proxy_protocol_filter.done = FALSE;
	li_proxy_protocol_data_init(&con->info.proxy_prot_data);
	li_stream_init(&con->proxy_protocol_filter.stream, &con->wrk->loop, proxy_prot_cb);
}

/* proxy protocol filter:
 * https://github.com/haproxy/haproxy/blob/master/doc/proxy-protocol.txt
 */

#ifndef _LIGHTTPD_PROXY_PROTOCOL_H_
#define _LIGHTTPD_PROXY_PROTOCOL_H_

#ifndef _LIGHTTPD_BASE_H_
#error Please include <lighttpd/base.h> instead of this file
#endif

typedef enum {
	LI_PROXY_PROT_TRANSPORT_UNSPEC = 0x00,
	LI_PROXY_PROT_TRANSPORT_STREAM = 0x01,
	LI_PROXY_PROT_TRANSPORT_DGRAM = 0x02,
} liProxyProtTransport;
#define LI_PROXY_PROT_TRANSPORT_MAX ((unsigned char)LI_PROXY_PROT_TRANSPORT_DGRAM)

typedef struct liProxyProtocolData liProxyProtocolData;
struct liProxyProtocolData {
	guint version;
	liProxyProtTransport transport;
	liSocketAddress remote;
	liSocketAddress local;

	guint skip_bytes;
	guint remaining_tlv_bytes;

	GByteArray *tlvs;
};

LI_API void li_proxy_protocol_data_init(liProxyProtocolData *data);
LI_API void li_proxy_protocol_data_clear(liProxyProtocolData *data);

typedef enum {
	LI_PROXY_PROTOCOL_PARSE_NEED_MORE_DATA = -2,
	LI_PROXY_PROTOCOL_PARSE_ERROR = -1,
	LI_PROXY_PROTOCOL_PARSE_DONE = 0,
} liProxyProtocolParseResult;

LI_API liProxyProtocolParseResult li_proxy_protocol_parse(
	liVRequest *vr,  /* probably con->mainvr, needed for logging and options */
	liProxyProtocolData *data,
	unsigned char *header,
	gssize header_len
);


/* liConnection integration */
typedef struct liConnectionProxyProtocolFilter liConnectionProxyProtocolFilter;
struct liConnectionProxyProtocolFilter {
	liStream stream;
	gboolean done;
};

LI_API void li_connection_proxy_protocol_init(liConnection *con);

#endif

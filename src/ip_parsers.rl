
#include <lighttpd/ip_parsers.h>

%%{
	machine ipv4_parser;

	action dc_start { tmpval = 0; }
	action dc_step { tmpval = 10*tmpval + (fc - '0'); }

	decint = (digit digit**) >dc_start $dc_step;
	octet = decint %{
		if (tmpval > 255) { res = FALSE; fbreak; }
		data[i++] = tmpval;
	};
	ipv4_data = octet "." octet "." octet "." octet;
	netmask = "/" decint %{
		if (tmpval > 32) { res = FALSE; fbreak; }
		*netmask = htonl(~((1 << (32-tmpval)) - 1));
	};
	port = ":" decint %{
		if (tmpval > 65535) { res = FALSE; fbreak; }
		*port = tmpval;
	};
	# so we don't need pe/eof vars
	end = (space | 0) @{ cs = ipv4_parser_first_final; fbreak; };

	ipv4 := ipv4_data end;
	ipv4_cidr := ipv4_data netmask end;
	ipv4_socket := ipv4_data port end;
	ipv4_socket_cidr := ipv4_data netmask port end;

	write data;
}%%

gboolean parse_ipv4(const char *str, guint32 *ip, guint32 *netmask, guint16 *port) {
	guint8 *data = (guint8*) ip;
	const char *p = str;
	gboolean res = TRUE;
	int cs, tmpval = 0, i = 0;

	%% write init nocs;

	cs = netmask
		? (port ? ipv4_parser_en_ipv4_socket_cidr : ipv4_parser_en_ipv4_cidr)
		: (port ? ipv4_parser_en_ipv4_socket      : ipv4_parser_en_ipv4);
	%% write exec noend;

	return res && cs >= ipv4_parser_first_final;
}

%%{
	machine ipv6_parser;

	action mark { mark = fpc; }

	action dc_start { tmpval = 0; }
	action dc_step { tmpval = 10*tmpval + (fc - '0'); }

	decint = (digit digit**) >dc_start $dc_step;
	octet = decint %{
		if (tmpval > 255) { res = FALSE; fbreak; }
		data[i++] = tmpval;
	};
	network = "/" decint %{
		if (tmpval > 128) { res = FALSE; fbreak; }
		*network = tmpval;
	};
	port = ":" decint %{
		if (tmpval > 65535) { res = FALSE; fbreak; }
		*port = tmpval;
	};

	hexint = (xdigit{1,4}) >mark  %{ tmpval = strtol(mark, NULL, 16); };
	group = hexint %{
		if (tmpval > 0xffff) { res = FALSE; fbreak; }
	};
	pregroup = group %{
		if (prec >= 8) { res = FALSE; fbreak; }
		predata[prec++] = htons(tmpval);
	};
	postgroup = group %{
		if (postc >= 8) { res = FALSE; fbreak; }
		postdata[postc++] = htons(tmpval);
	};
	ipv4_data = octet "." octet "." octet "." octet;
	pre_ipv4 = ipv4_data %{
		if (prec > 6) { res = FALSE; fbreak; }
		predata[prec++] = *(guint16*) (data);
		predata[prec++] = *(guint16*) (data+2);
	};
	post_ipv4 = ipv4_data %{
		if (postc > 6) { res = FALSE; fbreak; }
		postdata[postc++] = *(guint16*) (data);
		postdata[postc++] = *(guint16*) (data+2);
	};

	ipv6_data = ((pregroup ":")+ | ":") ((":" @ { compressed = TRUE; } (postgroup ":")* (postgroup | post_ipv4)?) | (pregroup | pre_ipv4));

	# so we don't need pe/eof vars
	end = (space | 0) @{ cs = ipv6_parser_first_final; fbreak; };

	ipv6 := ( ( ipv6_data ) | ( "[" ipv6_data "]" ) ) end;
	ipv6_cidr := ( ( ipv6_data network? ) | ( "[" ipv6_data network? "]" ) ) end;

	ipv6_socket := ( ( ipv6_data ) | ( "[" ipv6_data "]" port? ) ) end;
	ipv6_socket_cidr := ( ( ipv6_data network? ) | ( "[" ipv6_data network? "]" port?) ) end;

	write data;
}%%

gboolean parse_ipv6(const char *str, guint8 *ip, guint *network, guint16 *port) {
	guint8 data[4] = {0,0,0,0};
	guint16 *predata = (guint16*) ip, postdata[8];
	size_t prec = 0, postc = 0;
	const char *p = str, *mark = NULL;
	gboolean res = TRUE, compressed = FALSE;
	int cs, tmpval = 0, i = 0;

	%% write init nocs;

	cs = network
		? (port ? ipv6_parser_en_ipv6_socket_cidr : ipv6_parser_en_ipv6_cidr)
		: (port ? ipv6_parser_en_ipv6_socket      : ipv6_parser_en_ipv6);
	%% write exec noend;

	if (!res || cs < ipv6_parser_first_final) return FALSE;
	if (!compressed) return (prec == 8);
	if (prec + postc > 7) return FALSE;
	for ( ; prec < 8-postc; prec++) predata[prec] = 0;
	for (postc = 0 ; prec < 8; prec++, postc++ ) predata[prec] = postdata[postc];

	return TRUE;
}

GString* ipv6_tostring(GString *dest, const guint8 ip[16]) {
#define IPV6_TEMPLATE "ffff:ffff:ffff:ffff:ffff:ffff:abc.def.ghi.jkl"
	guint16 *data = (guint16*) ip;
	size_t i;

#ifdef HAVE_INET_NTOP
	g_string_set_size(dest, sizeof(IPV6_TEMPLATE)-1);
	if (inet_ntop(AF_INET6, ip, dest->str, dest->len)) {
		g_string_set_size(dest, strlen(dest->str));
		return dest;
	}
#endif
	g_string_truncate(dest, 0);
	g_string_printf(dest, "%" G_GINT16_MODIFIER "x", ntohs(data[0]));
	for (i = 1; i < 8; i++) {
		g_string_append_printf(dest, ":%" G_GINT16_MODIFIER "x", ntohs(data[i]));
	}
	return dest;
}

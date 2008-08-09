
#include "settings.h"

#include <stdlib.h>

%%{
	machine ipv4_parser;

	action mark { mark = fpc; }

	decint = (digit+) >mark  %{ tmpval = strtol(mark, NULL, 10); };
	octet = decint %{
		if (tmpval > 255) fbreak;
		data[i++] = tmpval;
	};
	ipv4_data = octet "." octet "." octet "." octet;

	ipv4 := ipv4_data (space|0) @{ res = TRUE; };
	ipv4_cidr := ipv4_data "/" decint (space|0) @{
		if (tmpval > 32) fbreak;
		*netmask = htonl(~((1 << (32-tmpval)) - 1));
		res = TRUE;
		fbreak;
	};

	write data;
}%%

gboolean parse_ipv4(const char *str, guint32 *ip, guint32 *netmask) {
	guint8 *data = (guint8*) ip;
	const char *p = str, *mark = NULL;
	gboolean res = FALSE;
	int cs, tmpval = 0, i = 0;

	%% write init nocs;

	cs = netmask ? ipv4_parser_en_ipv4_cidr : ipv4_parser_en_ipv4;
	%% write exec noend;

	return res;
}

%%{
	machine ipv6_parser;

	action mark { mark = fpc; }

	decint = (digit+) >mark  %{ tmpval = strtol(mark, NULL, 10); };
	octet = decint %{
		if (tmpval > 255) fbreak;
		data[i++] = tmpval;
	};

	hexint = (xdigit{1,4}) >mark  %{ tmpval = strtol(mark, NULL, 16); };
	group = hexint %{
		if (tmpval > 0xffff) fbreak;
	};
	pregroup = group %{
		if (prec >= 8) fbreak;
		predata[prec++] = htons(tmpval);
	};
	postgroup = group %{
		if (postc >= 8) fbreak;
		postdata[postc++] = htons(tmpval);
	};
	ipv4_data = octet "." octet "." octet "." octet;
	pre_ipv4 = ipv4_data %{
		if (prec > 6) fbreak;
		predata[prec++] = *(guint16*) (data);
		predata[prec++] = *(guint16*) (data+2);
	};
	post_ipv4 = ipv4_data %{
		if (postc > 6) fbreak;
		postdata[postc++] = *(guint16*) (data);
		postdata[postc++] = *(guint16*) (data+2);
	};

	ipv6_data = ((pregroup ":")+ | ":") ((":" @ { compressed = TRUE; } (postgroup ":")* (postgroup | post_ipv4)?) | (pregroup | pre_ipv4));

	ipv6 := ipv6_data (space|0) @{ res = TRUE; };
	ipv6_cidr := ipv6_data "/" decint (space|0) @{
		if (tmpval > 128) fbreak;
		*network = tmpval;
		res = TRUE;
		fbreak;
	};

	write data;
}%%

gboolean parse_ipv6(const char *str, guint8 *ip, guint *network) {
	guint8 data[4] = {0,0,0,0};
	guint16 *predata = (guint16*) ip, postdata[8];
	size_t prec = 0, postc = 0;
	const char *p = str, *mark = NULL;
	gboolean res = FALSE, compressed = FALSE;
	int cs, tmpval = 0, i = 0;

	%% write init nocs;

	cs = network ? ipv6_parser_en_ipv6_cidr : ipv6_parser_en_ipv6;
	%% write exec noend;

	if (!res) return FALSE;
	if (!compressed) return (prec == 8);
	if (prec + postc > 7) return FALSE;
	for ( ; prec < 8-postc; prec++) predata[prec] = 0;
	for (postc = 0 ; prec < 8; prec++, postc++ ) predata[prec] = postdata[postc];

	return TRUE;
}

GString* ipv6_tostring(const guint8 ip[16]) {
	guint16 *data = (guint16*) ip;
	size_t i;
	GString *s = g_string_sized_new(0);

	g_string_printf(s, "%" G_GINT16_MODIFIER "x", ntohs(data[0]));
	for (i = 1; i < 8; i++) {
		g_string_append_printf(s, ":%" G_GINT16_MODIFIER "x", ntohs(data[i]));
	}
	return s;
}

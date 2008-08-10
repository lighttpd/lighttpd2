
#include "url_parser.h"

#include <stdlib.h>

%%{
	machine url_parser;

	action mark { mark = fpc; }
	action mark_host { host_mark = fpc; }

	action save_host {
		g_string_truncate(uri->host, 0);
		g_string_append_len(uri->host, host_mark, fpc - host_mark);
		g_string_ascii_down(uri->host);
	}
	action save_authority {
		g_string_truncate(uri->authority, 0);
		g_string_append_len(uri->authority, mark, fpc - mark);
		g_string_ascii_down(uri->authority);
	}
	action save_path {
		g_string_append_len(uri->path, mark, fpc - mark);
	}
	action save_query {
		g_string_append_len(uri->query, mark, fpc - mark);
	}

	pct_encoded = "%" xdigit xdigit;

	gen_delims  = ":" | "/" | "?" | "#" | "[" | "]" | "@";
	sub_delims  = "!" | "$" | "&" | "'" | "(" | ")"
                  | "*" | "+" | "," | ";" | "=";

	reserved    = gen_delims | sub_delims;
	unreserved  = alpha | digit | "-" | "." | "_" | "~";


	pchar = unreserved | pct_encoded | sub_delims | ":" | "@";
	path = ("/" ( "/" | pchar)*) >mark %save_path;

#	scheme      = alpha *( alpha | digit | "+" | "-" | "." );
	scheme = "http" | "https";

#simple ipv4 address
	dec_octet = digit{1,3};
	IPv4address = dec_octet "." dec_octet "." dec_octet "." dec_octet;

	IPvFuture  = "v" xdigit+ "." ( unreserved | sub_delims | ":" )+;

# simple ipv6 address
	IPv6address = (":" | xdigit)+ IPv4address?;

	IP_literal = "[" ( IPv6address | IPvFuture  ) "]";

	reg_name = ( unreserved | pct_encoded | sub_delims )+;

	userinfo    = ( unreserved | pct_encoded | sub_delims | ":" )*;
	host        = IP_literal | IPv4address | reg_name;
	port        = digit+;
	authority   = ( userinfo "@" )? (host >mark_host %save_host) ( ":" port )?;

	query = ( pchar | "/" | "?" )* >mark %save_query;
	fragment = ( pchar | "/" | "?" )*;

	URI_path = path ( "?" query )? ( "#" fragment )?;

	URI = scheme "://" (authority >mark %save_authority) URI_path;

	parse_URI := URI | ("*" >mark %save_path) | URI_path;
	parse_Hostname := (host >mark_host %save_host) ( ":" port )?;

	write data;
}%%

gboolean parse_raw_url(request_uri *uri) {
	const char *p, *pe, *eof;
	const char *mark = NULL, *host_mark = NULL;
	int cs;

	p = uri->raw->str;
	eof = pe = uri->raw->str + uri->raw->len;

	%% write init nocs;
	cs = url_parser_en_parse_URI;

	%% write exec;

	return (cs >= url_parser_first_final);
}

gboolean parse_hostname(request_uri *uri) {
	const char *p, *pe, *eof;
	const char *mark = NULL, *host_mark = NULL;
	int cs;

	g_string_ascii_down(uri->authority);
	p = uri->authority->str;
	eof = pe = uri->authority->str + uri->authority->len;

	%% write init nocs;
	cs = url_parser_en_parse_Hostname;

	%% write exec;

	return (cs >= url_parser_first_final);
}

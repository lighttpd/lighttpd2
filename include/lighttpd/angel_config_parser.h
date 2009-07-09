#ifndef _LIGHTTPD_ANGEL_CONIG_PARSER_H_
#define _LIGHTTPD_ANGEL_CONIG_PARSER_H_

/* error handling */
#define LI_ANGEL_CONFIG_PARSER_ERROR li_angel_config_parser_error_quark()
LI_API GQuark li_angel_config_parser_error_quark();

typedef enum {
	LI_ANGEL_CONFIG_PARSER_ERROR_PARSE,         /* parse error */
} liAngelConfigParserError;

LI_API gboolean li_angel_config_parse_file(liServer *srv, const gchar *filename, GError **err);

#endif

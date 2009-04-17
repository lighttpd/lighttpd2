#ifndef _LIGHTTPD_ANGEL_CONIG_PARSER_H_
#define _LIGHTTPD_ANGEL_CONIG_PARSER_H_

/* error handling */
#define ANGEL_CONFIG_PARSER_ERROR angel_config_parser_error_quark()
LI_API GQuark angel_config_parser_error_quark();

typedef enum {
	ANGEL_CONFIG_PARSER_ERROR_PARSE,         /* parse error */
} AngelConfigParserError;

LI_API gboolean angel_config_parse_file(const gchar *filename, GError **err);

#endif

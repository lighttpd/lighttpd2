#ifndef _LIGHTTPD_CONFIGPARSER_H_
#define _LIGHTTPD_CONFIGPARSER_H_

#include <lighttpd/base.h>

#define LI_CONFIG_ERROR li_config_error_quark()
LI_API GQuark li_config_error_quark(void);

LI_API gboolean li_config_parse(liServer *srv, const gchar *config_path);

/* parse more config snippets at runtime. does not support includes and modifying global vars */
LI_API liAction* li_config_parse_live(liWorker *wrk, const gchar *sourcename, const char *source, gsize sourcelen, GError **error);

#endif

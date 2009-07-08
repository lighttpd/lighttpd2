#ifndef _LIGHTTPD_ANGEL_BASE_H_
#define _LIGHTTPD_ANGEL_BASE_H_

#ifdef _LIGHTTPD_BASE_H_
#error Do not mix lighty with angel code
#endif

#include <lighttpd/settings.h>

#include <lighttpd/module.h>

/* angel_server.h */

typedef struct liServer liServer;
typedef struct liInstance liInstance;
typedef struct liInstanceConf liInstanceConf;


#include <lighttpd/angel_value.h>
#include <lighttpd/angel_data.h>
#include <lighttpd/angel_connection.h>
#include <lighttpd/angel_log.h>
#include <lighttpd/angel_plugin.h>
#include <lighttpd/angel_server.h>

#include <lighttpd/utils.h>

#endif

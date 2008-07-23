#ifndef _LIGHTTPD_BASE_H_
#define _LIGHTTPD_BASE_H_

#include "settings.h"

#define CONST_STR_LEN(x) x, x ? sizeof(x) - 1 : 0

struct server;
typedef struct server server;

struct connection;
typedef struct connection connection;


#include "server.h"
#include "actions.h"
#include "plugin.h"
#include "request.h"
#include "log.h"

#include "connection.h"

#define SERVER_VERSION ((guint) 0x01FF0000)

#endif

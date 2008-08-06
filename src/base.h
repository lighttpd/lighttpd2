#ifndef _LIGHTTPD_BASE_H_
#define _LIGHTTPD_BASE_H_

#include "settings.h"

#define CONST_STR_LEN(x) x, x ? sizeof(x) - 1 : 0

#define GSTR_LEN(x) x ? x->str : "", x ? x->len : 0

typedef enum {
	HTTP_TRANSFER_ENCODING_IDENTITY,
	HTTP_TRANSFER_ENCODING_CHUNKED
} transfer_encoding_t;

struct server;
typedef struct server server;

struct connection;
typedef struct connection connection;


#include "server.h"
#include "actions.h"
#include "options.h"
#include "plugin.h"
#include "request.h"
#include "response.h"
#include "log.h"

#include "connection.h"

#define SERVER_VERSION ((guint) 0x01FF0000)

#endif

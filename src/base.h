#ifndef _LIGHTTPD_BASE_H_
#define _LIGHTTPD_BASE_H_

#include "settings.h"

#define CONST_STR_LEN(x) (x), (x) ? sizeof(x) - 1 : 0

#define GSTR_LEN(x) (x) ? (x)->str : "", (x) ? (x)->len : 0

/* we don't use ev_init for now (stupid alias warnings), as ev_init
 * just does set some values to zero and calls ev_set_cb.
 * But every structure we allacote is initialized with zero, so we don't care
 * about that.
 * If this ever changes, we can easily use ev_init again.
 */
#define my_ev_init(ev, cb) ev_set_cb(ev, cb)

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

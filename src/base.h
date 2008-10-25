#ifndef _LIGHTTPD_BASE_H_
#define _LIGHTTPD_BASE_H_

#include "settings.h"

/* Next try to fix strict-alias warning */
#undef ev_init
#define ev_init(ev,cb_) do {                   \
  ev_watcher *ew = (ev_watcher *)(void *)ev;   \
  (ew)->active   =                             \
  (ew)->pending  =                             \
  (ew)->priority = 0;                          \
  ev_set_cb ((ev), cb_);                       \
} while (0)

#undef ev_timer_set
#define ev_timer_set(ev,after_,repeat_) do {   \
  ev_watcher_time *ew = (ev_watcher_time *)(ev); \
  ew->at = (after_);                           \
  (ev)->repeat = (repeat_);                    \
} while (0)

#define CONST_STR_LEN(x) (x), (x) ? sizeof(x) - 1 : 0

#define GSTR_LEN(x) (x) ? (x)->str : "", (x) ? (x)->len : 0
#define GSTR_SAFE_STR(x) ((x && x->str) ? x->str : "(null)")

typedef enum {
	HTTP_TRANSFER_ENCODING_IDENTITY,
	HTTP_TRANSFER_ENCODING_CHUNKED
} transfer_encoding_t;

struct server;
typedef struct server server;

struct connection;
typedef struct connection connection;


#include "server.h"
#include "worker.h"
#include "actions.h"
#include "options.h"
#include "plugin.h"
#include "request.h"
#include "response.h"
#include "virtualrequest.h"
#include "log.h"

#include "connection.h"

#include "utils.h"

#define SERVER_VERSION ((guint) 0x01FF0000)

#endif

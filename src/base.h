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

#include "typedefs.h"
#include "utils.h"

#include "module.h"

#include "chunk.h"
#include "chunk_parser.h"

#include "server.h"
#include "worker.h"
#include "condition.h"
#include "options.h"
#include "value.h"
#include "actions.h"
#include "plugin.h"
#include "http_headers.h"
#include "http_request_parser.h"
#include "request.h"
#include "response.h"
#include "virtualrequest.h"
#include "log.h"

#include "connection.h"

#include "network.h"
#include "collect.h"
#include "utils.h"

#define SERVER_VERSION ((guint) 0x01FF0000)

#endif

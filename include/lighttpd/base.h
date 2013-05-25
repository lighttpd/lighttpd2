#ifndef _LIGHTTPD_BASE_H_
#define _LIGHTTPD_BASE_H_

#ifdef _LIGHTTPD_ANGEL_BASE_H_
#error Do not mix lighty with angel code
#endif

#include <lighttpd/settings.h>

#include <lighttpd/typedefs.h>
#include <lighttpd/module.h>

#include <lighttpd/angel_data.h>
#include <lighttpd/angel_connection.h>

#include <lighttpd/buffer.h>
#include <lighttpd/chunk.h>
#include <lighttpd/chunk_parser.h>

#include <lighttpd/waitqueue.h>
#include <lighttpd/stream.h>
#include <lighttpd/filter.h>
#include <lighttpd/filter_chunked.h>
#include <lighttpd/radix.h>

#include <lighttpd/base_lua.h>
#include <lighttpd/log.h>
#include <lighttpd/server.h>
#include <lighttpd/worker.h>
#include <lighttpd/angel.h>
#include <lighttpd/condition.h>
#include <lighttpd/ip_parsers.h>
#include <lighttpd/options.h>
#include <lighttpd/value.h>
#include <lighttpd/actions.h>
#include <lighttpd/plugin.h>
#include <lighttpd/http_headers.h>
#include <lighttpd/http_request_parser.h>
#include <lighttpd/http_response_parser.h>
#include <lighttpd/request.h>
#include <lighttpd/response.h>
#include <lighttpd/environment.h>
#include <lighttpd/virtualrequest.h>
#include <lighttpd/stat_cache.h>
#include <lighttpd/mimetype.h>

#include <lighttpd/connection.h>

#include <lighttpd/collect.h>
#include <lighttpd/network.h>
#include <lighttpd/etag.h>
#include <lighttpd/utils.h>

#define SERVER_VERSION ((guint) 0x01FF0000)

#endif

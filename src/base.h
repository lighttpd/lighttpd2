#ifndef _LIGHTTPD_BASE_H_
#define _LIGHTTPD_BASE_H_

#include "settings.h"

#define CONST_STR_LEN(x) x, x ? sizeof(x) - 1 : 0

struct server;
typedef struct server server;

struct connection;
typedef struct connection connection;

#include "plugin.h"
#include "actions.h"
#include "request.h"
#include "log.h"

#define SERVER_VERSION ((guint) 0x01FF0000)

struct server {
	guint version;

	GHashTable *plugins;

	size_t option_count;
	GHashTable *options;
	gpointer *option_def_values;

	gboolean exiting;
	GMutex *mutex;

	gboolean rotate_logs;
	GHashTable *logs;
	struct log_t *log_stderr;
	struct log_t *log_syslog;
	GAsyncQueue *log_queue;
	GThread *log_thread;
};

struct connection {

	sock_addr dst_addr, src_addr;
	GString *dst_addr_str, *src_addr_str;

	action_stack action_stack;

	request request;
	physical physical;

	GMutex *mutex;

	struct log_t *log;
	gint log_level;
};

server* server_new();
void server_free(server* srv);

#endif

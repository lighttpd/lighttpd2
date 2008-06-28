#ifndef _LIGHTTPD_BASE_H_
#define _LIGHTTPD_BASE_H_

#include "settings.h"

#define CONST_STR_LEN(x) x, x ? sizeof(x) - 1 : 0

struct server;
typedef struct server server;

struct connection;
typedef struct connection connection;

#include "plugin.h"

struct server {
	GHashTable *plugins;

	size_t option_count;
	GHashTable *options;
	gpointer *option_def_values;
};

#endif

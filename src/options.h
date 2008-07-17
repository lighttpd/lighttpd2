#ifndef _LIGHTTPD_OPTIONS_H_
#define _LIGHTTPD_OPTIONS_H_

typedef enum {
	OPTION_NONE,
	OPTION_BOOLEAN,
	OPTION_INT,
	OPTION_STRING,
	OPTION_LIST,
	OPTION_HASH
} option_type;

struct option;
typedef struct option option;

struct option_set;
typedef struct option_set option_set;

#include "settings.h"

struct option {
	option_type type;
	union {
		gboolean opt_bool;
		gint opt_int;
		GString *opt_string;
		/* array of option */
		GArray *opt_list;
		/* hash GString => option */
		GHashTable *opt_hash;
	} value;
};


struct server_option;
struct option_set {
	size_t ndx;
	gpointer value;
	struct server_option *sopt;
};

LI_API option* option_new_bool(gboolean val);
LI_API option* option_new_int(gint val);
LI_API option* option_new_string(GString *val);
LI_API option* option_new_list();
LI_API option* option_new_hash();
LI_API void option_free(option* opt);

LI_API const char* option_type_string(option_type type);

LI_API void option_list_free(GArray *optlist);

/* Extract value from option, destroy option */
LI_API gpointer option_extract_value(option *opt);


gboolean option_get_index(server *srv, GString *name, gsize *ndx);
#endif

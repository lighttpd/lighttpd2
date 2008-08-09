#ifndef _LIGHTTPD_OPTIONS_H_
#define _LIGHTTPD_OPTIONS_H_

typedef enum {
	OPTION_NONE,
	OPTION_BOOLEAN,
	OPTION_INT,
	OPTION_STRING,
	OPTION_LIST,
	OPTION_HASH,
	OPTION_ACTION,     /**< shouldn't be used for options, but may be needed for constructing actions */
	OPTION_CONDITION   /**< shouldn't be used for options, but may be needed for constructing actions */
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
		struct {
			server *srv;    /* needed for destruction */
			action *action;
		} opt_action;
		struct {
			server *srv;    /* needed for destruction */
			condition *cond;
		} opt_cond;
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
LI_API option* option_new_action(server *srv, action *a);
LI_API option* option_new_condition(server *srv, condition *c);
LI_API void option_free(option* opt);

LI_API const char* option_type_string(option_type type);

LI_API void option_list_free(GArray *optlist);

/* Extract value from option, option set to none */
LI_API gpointer option_extract_value(option *opt);

#endif

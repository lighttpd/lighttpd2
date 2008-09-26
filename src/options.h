#ifndef _LIGHTTPD_OPTIONS_H_
#define _LIGHTTPD_OPTIONS_H_

struct option_set;
typedef struct option_set option_set;

union option_value;
typedef union option_value option_value;

#include "settings.h"
#include "value.h"

union option_value {
	gpointer ptr;
	gint64 number;
	gboolean boolean;

/* some common pointer types */
	GString *string;
	GArray *list;
	GHashTable *hash;
	action *action;
	condition *cond;
};

struct server_option;
struct option_set {
	size_t ndx;
	option_value value;
	struct server_option *sopt;
};

/* Extract content from value, value set to none */
LI_API option_value value_extract(value *val);

LI_API gpointer value_extract_ptr(value *val);
LI_API gint64 value_extract_number(value *val);
LI_API gboolean value_extract_bool(value *val);

#endif

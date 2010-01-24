#ifndef _LIGHTTPD_OPTIONS_H_
#define _LIGHTTPD_OPTIONS_H_

#ifndef _LIGHTTPD_BASE_H_
#error Please include <lighttpd/base.h> instead of this file
#endif

union liOptionValue {
	gpointer ptr;
	gint64 number;
	gboolean boolean;

/* some common pointer types */
	GString *string;
	GArray *list;
	GHashTable *hash;
	liAction *action;
	liCondition *cond;
};

struct liOptionSet {
	size_t ndx;
	liOptionValue value;
	liServerOption *sopt;
};

/* Extract content from value, value set to none */
LI_API liOptionValue li_value_extract(liValue *val);

#endif

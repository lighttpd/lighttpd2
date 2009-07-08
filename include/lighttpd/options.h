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
LI_API liOptionValue value_extract(liValue *val);

LI_API gpointer value_extract_ptr(liValue *val);
LI_API gint64 value_extract_number(liValue *val);
LI_API gboolean value_extract_bool(liValue *val);

#endif

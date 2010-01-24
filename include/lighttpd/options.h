#ifndef _LIGHTTPD_OPTIONS_H_
#define _LIGHTTPD_OPTIONS_H_

#ifndef _LIGHTTPD_BASE_H_
#error Please include <lighttpd/base.h> instead of this file
#endif

union liOptionValue {
	gint64 number;
	gboolean boolean;
};

struct liOptionPtrValue {
	gint refcount;

	union {
		gpointer ptr;

/* some common pointer types */
		GString *string;
		GArray *list;
		GHashTable *hash;
		liAction *action;
		liCondition *cond;
	} data;

	liServerOptionPtr *sopt;
};

struct liOptionSet {
	size_t ndx;
	liOptionValue value;
};

struct liOptionPtrSet {
	size_t ndx;
	liOptionPtrValue *value;
};

#endif

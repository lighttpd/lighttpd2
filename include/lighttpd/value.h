#ifndef _LIGHTTPD_VALUE_H_
#define _LIGHTTPD_VALUE_H_

#ifndef _LIGHTTPD_BASE_H_
#error Please include <lighttpd/base.h> instead of this file
#endif

struct liValue {
	liValueType type;
	union {
		gboolean boolean;
		gint64 number;
		GString *string;
		/* array of (liValue*) */
		GArray *list;
		/* hash GString => value */
		GHashTable *hash;
		struct {
			liServer *srv;    /* needed for destruction */
			liAction *action;
		} val_action;
		struct {
			liServer *srv;    /* needed for destruction */
			liCondition *cond;
		} val_cond;
	} data;
};

LI_API liValue* value_new_none();
LI_API liValue* value_new_bool(gboolean val);
LI_API liValue* value_new_number(gint64 val);
LI_API liValue* value_new_string(GString *val);
LI_API liValue* value_new_list();
LI_API liValue* value_new_hash();
LI_API liValue* value_new_action(liServer *srv, liAction *a);
LI_API liValue* value_new_condition(liServer *srv, liCondition *c);

LI_API liValue* value_copy(liValue* val);
LI_API void value_free(liValue* val);

LI_API const char* value_type_string(liValueType type);

LI_API GString *value_to_string(liValue *val);

LI_API void value_list_free(GArray *vallist);

#endif

#ifndef _LIGHTTPD_VALUE_H_
#define _LIGHTTPD_VALUE_H_

typedef enum {
	VALUE_NONE,
	VALUE_BOOLEAN,
	VALUE_NUMBER,
	VALUE_STRING,
	VALUE_LIST,
	VALUE_HASH,
	VALUE_ACTION,     /**< shouldn't be used for options, but may be needed for constructing actions */
	VALUE_CONDITION   /**< shouldn't be used for options, but may be needed for constructing actions */
} value_type;

struct value;
typedef struct value value;

#include "settings.h"

struct value {
	value_type type;
	union {
		gboolean boolean;
		gint64 number;
		GString *string;
		/* array of value */
		GArray *list;
		/* hash GString => value */
		GHashTable *hash;
		struct {
			server *srv;    /* needed for destruction */
			action *action;
		} val_action;
		struct {
			server *srv;    /* needed for destruction */
			condition *cond;
		} val_cond;
	} data;
};

LI_API value* value_new_bool(gboolean val);
LI_API value* value_new_number(gint64 val);
LI_API value* value_new_string(GString *val);
LI_API value* value_new_list();
LI_API value* value_new_hash();
LI_API value* value_new_action(server *srv, action *a);
LI_API value* value_new_condition(server *srv, condition *c);

LI_API value* value_copy(value* val);
LI_API void value_free(value* val);

LI_API const char* value_type_string(value_type type);

LI_API GString *value_to_string(value *val);

LI_API void value_list_free(GArray *vallist);

#endif

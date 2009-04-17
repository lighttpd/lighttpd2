#ifndef _LIGHTTPD_ANGEL_VALUE_H_
#define _LIGHTTPD_ANGEL_VALUE_H_

#ifndef _LIGHTTPD_ANGEL_BASE_H_
#error Please include <lighttpd/angel_base.h> instead of this file
#endif

struct value;
typedef struct value value;

struct value_range;
typedef struct value_range value_range;

typedef enum {
	VALUE_NONE,
/* primitive types */
	VALUE_BOOLEAN,
	VALUE_NUMBER,
	VALUE_STRING,
	VALUE_RANGE,

/* container */
	VALUE_LIST,
	VALUE_HASH
} value_type;

struct value_range {
	guint64 from, to;
};

struct value {
	value_type type;
	union {
		gboolean boolean;
		gint64 number;
		GString *string;
		value_range range;
		/* array of (value*) */
		GPtrArray *list;
		/* hash GString => value */
		GHashTable *hash;
	} data;
};

LI_API value* value_new_none();
LI_API value* value_new_bool(gboolean val);
LI_API value* value_new_number(gint64 val);
LI_API value* value_new_string(GString *val);
LI_API value* value_new_range(value_range val);
LI_API value* value_new_list();
LI_API value* value_new_hash();

LI_API value* value_copy(value* val);
LI_API void value_free(value* val);

LI_API const char* value_type_string(value_type type);

LI_API GString *value_to_string(value *val);

#endif

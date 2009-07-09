#ifndef _LIGHTTPD_ANGEL_VALUE_H_
#define _LIGHTTPD_ANGEL_VALUE_H_

#ifndef _LIGHTTPD_ANGEL_BASE_H_
#error Please include <lighttpd/angel_base.h> instead of this file
#endif

typedef struct liValue liValue;

typedef struct liValueRange liValueRange;

typedef enum {
	LI_VALUE_NONE,
/* primitive types */
	LI_VALUE_BOOLEAN,
	LI_VALUE_NUMBER,
	LI_VALUE_STRING,
	LI_VALUE_RANGE,

/* container */
	LI_VALUE_LIST,
	LI_VALUE_HASH
} liValueType;

struct liValueRange {
	guint64 from, to;
};

struct liValue {
	liValueType type;
	union {
		gboolean boolean;
		gint64 number;
		GString *string;
		liValueRange range;
		/* array of (liValue*) */
		GPtrArray *list;
		/* hash GString => value */
		GHashTable *hash;
	} data;
};

LI_API liValue* li_value_new_none();
LI_API liValue* li_value_new_bool(gboolean val);
LI_API liValue* li_value_new_number(gint64 val);
LI_API liValue* li_value_new_string(GString *val);
LI_API liValue* li_value_new_range(liValueRange val);
LI_API liValue* li_value_new_list();
LI_API liValue* li_value_new_hash();

LI_API liValue* li_value_copy(liValue* val);
LI_API void li_value_free(liValue* val);

LI_API const char* li_value_type_string(liValueType type);

LI_API GString *li_value_to_string(liValue *val);

#endif

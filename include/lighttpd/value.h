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

LI_API liValue* li_value_new_none(void);
LI_API liValue* li_value_new_bool(gboolean val);
LI_API liValue* li_value_new_number(gint64 val);
LI_API liValue* li_value_new_string(GString *val);
LI_API liValue* li_value_new_list(void);
LI_API liValue* li_value_new_hash(void);
LI_API liValue* li_value_new_action(liServer *srv, liAction *a);
LI_API liValue* li_value_new_condition(liServer *srv, liCondition *c);

LI_API void li_value_list_append(liValue *list, liValue *item); /* list MUST be of type LIST */

/* wraps current content in a list with 1 entry */
LI_API void li_value_wrap_in_list(liValue *val);

LI_API liValue* li_value_copy(liValue* val);
LI_API void li_value_clear(liValue *val); /* frees content, sets value to LI_VALUE_NONE */
LI_API void li_value_free(liValue* val);
LI_API void li_value_move(liValue *dest, liValue *src);

LI_API const char* li_value_type_string(liValueType type);

LI_API GString *li_value_to_string(liValue *val);

LI_API void li_value_list_free(GArray *vallist);

/* extracts the pointer of a; set val->type to none (so a free on the value doesn't free the previous content)
 * returns NULL (and doesn't modify val->type) if the type doesn't match
 * there is no need to extract scalar values - just copy them
 */
LI_API gpointer li_value_extract_ptr(liValue *val);
LI_API GString* li_value_extract_string(liValue *val);
LI_API GArray* li_value_extract_list(liValue *val);
LI_API GHashTable* li_value_extract_hash(liValue *val);
LI_API liAction* li_value_extract_action(liValue *val);
LI_API liCondition* li_value_extract_condition(liValue *val);

/* move the value content to a new value, set the old type to none */
LI_API liValue* li_value_extract(liValue *val);

/* converts value type to LI_VALUE_LIST, makes sure list contains (key,value) tuples, and the keys are all LI_VALUE_STRING or LI_VALUE_NONE */
LI_API liValue* li_value_to_key_value_list(liValue *val);

#endif

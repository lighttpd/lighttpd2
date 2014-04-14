#ifndef _LIGHTTPD_VALUE_H_
#define _LIGHTTPD_VALUE_H_

#if !(defined _LIGHTTPD_BASE_H_ || defined _LIGHTTPD_ANGEL_BASE_H_)
# ifdef _LIGHTTPD_COMMON_VALUE_C_
#  include <lighttpd/settings.h>
# else
#  error Please include <lighttpd/base.h> or <lighttpd/angel_base.h> instead of this file
# endif
#endif

/* common code for values in angel and worker; struct liValue must always be of same size! */

typedef struct liValue liValue;

typedef enum
	{ LI_VALUE_NONE
	, LI_VALUE_BOOLEAN
	, LI_VALUE_NUMBER
	, LI_VALUE_STRING
	, LI_VALUE_LIST
#ifdef _LIGHTTPD_BASE_H_
	, LI_VALUE_ACTION      /**< shouldn't be used for options, but may be needed for constructing actions */
	, LI_VALUE_CONDITION   /**< shouldn't be used for options, but may be needed for constructing actions */
#endif
} liValueType;

struct liValue {
	liValueType type;
	union {
		gboolean boolean;
		gint64 number;
		GString *string;
		/* array of (liValue*) */
		GPtrArray *list;
		struct {
			struct liServer *srv;    /* needed for destruction */
			struct liAction *action;
		} val_action;
		struct {
			struct liServer *srv;    /* needed for destruction */
			struct liCondition *cond;
		} val_cond;
	} data;
};

LI_API liValue* li_value_new_none(void);
LI_API liValue* li_value_new_bool(gboolean val);
LI_API liValue* li_value_new_number(gint64 val);
LI_API liValue* li_value_new_string(GString *val);
LI_API liValue* li_value_new_list(void);
#ifdef _LIGHTTPD_BASE_H_
LI_API liValue* li_value_new_action(liServer *srv, liAction *a);
LI_API liValue* li_value_new_condition(liServer *srv, liCondition *c);
#endif
LI_API GHashTable *li_value_new_hashtable(void); /* returns a GString -> liValue table with free funcs */

LI_API void li_value_list_append(liValue *list, liValue *item); /* list MUST be of type LIST */

/* wraps current content in a list with 1 entry */
LI_API void li_value_wrap_in_list(liValue *val);

LI_API liValue* li_value_copy(liValue* val);
LI_API liValue* li_common_value_copy_(liValue* val); /* internal function */
LI_API void li_value_clear(liValue *val); /* frees content, sets value to LI_VALUE_NONE */
LI_API void li_common_value_clear_(liValue *val); /* internal function */
LI_API void li_value_free(liValue* val);
LI_API void li_value_move(liValue *dest, liValue *src);

LI_API const char* li_valuetype_string(liValueType type);
LI_API const char* li_common_valuetype_string_(liValueType type); /* internal function */
INLINE const char* li_value_type_string(liValue *val);

LI_API GString *li_value_to_string(liValue *val);
LI_API GString *li_common_value_to_string_(liValue *val); /* internal function */

LI_API void li_value_list_free(GPtrArray *vallist);

/* extracts the pointer of a; set val->type to none (so a free on the value doesn't free the previous content)
 * returns NULL (and doesn't modify val->type) if the type doesn't match
 * there is no need to extract scalar values - just copy them
 */
LI_API gpointer li_value_extract_ptr(liValue *val);
LI_API gpointer li_common_value_extract_ptr_(liValue *val); /* internal function */
LI_API GString* li_value_extract_string(liValue *val);
LI_API GPtrArray* li_value_extract_list(liValue *val);
#ifdef _LIGHTTPD_BASE_H_
LI_API liAction* li_value_extract_action(liValue *val);
LI_API liCondition* li_value_extract_condition(liValue *val);
#endif

/* move the value content to a new value, set the old type to none */
LI_API liValue* li_value_extract(liValue *val);

/* converts value type to LI_VALUE_LIST, makes sure list contains (key,value) tuples, and the keys are all LI_VALUE_STRING or NULL / LI_VALUE_NONE */
LI_API liValue* li_value_to_key_value_list(liValue *val);

/* if val is list with exactly one element, return the element. otherwise return val */
INLINE liValue* li_value_get_single_argument(liValue *val);

/* returns whether val == 0 || (val->type == NONE) || (val->type == LIST && 0 == val->list->len) */
INLINE gboolean li_value_is_nothing(liValue *val);

/* returns type of value or LI_VALUE_NONE for NULL */
INLINE liValueType li_value_type(liValue *val);

/* returns whether val is a list and has length len */
INLINE gboolean li_value_list_has_len(liValue *val, guint len);
/* returns length of list or 0 if not a list */
INLINE guint li_value_list_len(liValue *val);
/* returns entry of list or NULL if not a list or out of bounds */
INLINE liValue* li_value_list_at(liValue* val, guint ndx);
/* returns type of list entry, or LI_VALUE_NONE if not a list or NULL entry or out of bounds */
INLINE liValueType li_value_list_type_at(liValue *val, guint ndx);
/* set list entry at given index */
INLINE void li_value_list_set(liValue *val, guint ndx, liValue *entry);

#define LI_VALUE_FOREACH(entry, list) { \
	guint _ ## entry ## _i, _ ## entry ## _len = li_value_list_len(list); \
	for (_ ## entry ## _i = 0; _ ## entry ## _i < _ ## entry ## _len; ++ _ ## entry ## _i ) { \
		liValue *entry = li_value_list_at(list, _ ## entry ## _i);
#define LI_VALUE_END_FOREACH() } }

/* inline implementations */
INLINE const char* li_value_type_string(liValue *val) {
	return NULL == val ? "NULL" : li_valuetype_string(val->type);
}

INLINE liValue* li_value_get_single_argument(liValue *val) {
	return li_value_list_has_len(val, 1) ? li_value_list_at(val, 0) : val;
}

INLINE gboolean li_value_is_nothing(liValue *val) {
	return NULL == val || LI_VALUE_NONE == val->type || (LI_VALUE_LIST == val->type && 0 == val->data.list->len);
}

INLINE liValueType li_value_type(liValue *val) {
	return NULL == val ? LI_VALUE_NONE : val->type;
}

INLINE gboolean li_value_list_has_len(liValue *val, guint len) {
	return (NULL != val && LI_VALUE_LIST == val->type && len == val->data.list->len);
}

INLINE guint li_value_list_len(liValue *val) {
	return (NULL != val && LI_VALUE_LIST == val->type) ? val->data.list->len : 0;
}

INLINE liValue* li_value_list_at(liValue* val, guint ndx) {
	if (NULL == val || LI_VALUE_LIST != val->type || ndx >= val->data.list->len) return NULL;
	return g_ptr_array_index(val->data.list, ndx);
}

INLINE liValueType li_value_list_type_at(liValue *val, guint ndx) {
	return li_value_type(li_value_list_at(val, ndx));
}

INLINE void li_value_list_set(liValue *val, guint ndx, liValue *entry) {
	GPtrArray *list;
	if (NULL == val || LI_VALUE_LIST != val->type) {
		li_value_free(entry);
		return;
	}
	list = val->data.list;
	if (ndx <= list->len) {
		g_ptr_array_set_size(list, ndx + 1);
	}
	li_value_free(g_ptr_array_index(list, ndx));
	g_ptr_array_index(list, ndx) = entry;
}

#endif

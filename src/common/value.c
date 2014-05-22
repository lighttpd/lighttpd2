#define _LIGHTTPD_COMMON_VALUE_C_

#include <lighttpd/value.h>
#include <lighttpd/utils.h>

liValue* li_value_new_none(void) {
	liValue *v = g_slice_new0(liValue);
	v->type = LI_VALUE_NONE;
	return v;
}

liValue* li_value_new_bool(gboolean val) {
	liValue *v = g_slice_new0(liValue);
	v->data.boolean = val;
	v->type = LI_VALUE_BOOLEAN;
	return v;
}

liValue* li_value_new_number(gint64 val) {
	liValue *v = g_slice_new0(liValue);
	v->data.number = val;
	v->type = LI_VALUE_NUMBER;
	return v;
}

liValue* li_value_new_string(GString *val) {
	liValue *v = g_slice_new0(liValue);
	v->data.string = val;
	v->type = LI_VALUE_STRING;
	return v;
}

liValue* li_value_new_list(void) {
	liValue *v = g_slice_new0(liValue);
	v->data.list = g_ptr_array_new();
	v->type = LI_VALUE_LIST;
	return v;
}

void li_value_list_append(liValue *list, liValue *item) {
	LI_FORCE_ASSERT(LI_VALUE_LIST == list->type);
	g_ptr_array_add(list->data.list, item);
}

void li_value_wrap_in_list(liValue *val) {
	liValue *item;
	LI_FORCE_ASSERT(NULL != val);

	item = li_value_extract(val);
	val->type = LI_VALUE_LIST;
	val->data.list = g_ptr_array_new();
	g_ptr_array_add(val->data.list, item);
}

static void _li_value_clear(liValue *val) {
	memset(val, 0, sizeof(*val));
	val->type = LI_VALUE_NONE;
}

const char* li_common_valuetype_string_(liValueType type) {
	switch(type) {
	case LI_VALUE_NONE:
		return "none";
	case LI_VALUE_BOOLEAN:
		return "boolean";
	case LI_VALUE_NUMBER:
		return "number";
	case LI_VALUE_STRING:
		return "string";
	case LI_VALUE_LIST:
		return "list";
	}
	return "<unknown>";
}

gpointer li_common_value_extract_ptr_(liValue *val) {
	gpointer ptr = NULL;

	if (NULL == val) return NULL;

	switch (val->type) {
	case LI_VALUE_NONE:
		break;
	case LI_VALUE_BOOLEAN:
		break;
	case LI_VALUE_NUMBER:
		break;
	case LI_VALUE_STRING:
		ptr = val->data.string;
		break;
	case LI_VALUE_LIST:
		ptr = val->data.list;
		break;
	}
	_li_value_clear(val);
	return ptr;
}


GString* li_value_extract_string(liValue *val) {
	GString* result;
	if (NULL == val || val->type != LI_VALUE_STRING) return NULL;
	result = val->data.string;
	_li_value_clear(val);
	return result;
}

GPtrArray* li_value_extract_list(liValue *val) {
	GPtrArray* result;
	if (NULL == val || val->type != LI_VALUE_LIST) return NULL;
	result = val->data.list;
	_li_value_clear(val);
	return result;
}

liValue* li_value_extract(liValue *val) {
	liValue *v;
	if (NULL == val) return NULL;
	v = li_value_new_none();
	*v = *val;
	_li_value_clear(val);
	return v;
}

liValue* li_value_to_key_value_list(liValue *val) {
	if (NULL == val) return NULL;

	if (LI_VALUE_LIST == val->type) {
		if (li_value_list_has_len(val, 2) &&
				(LI_VALUE_STRING == li_value_list_type_at(val, 0) || LI_VALUE_NONE == li_value_list_type_at(val, 0))) {
			/* single key-value pair */
			li_value_wrap_in_list(val);
			return val;
		}

		/* verify key-value list properties */
		LI_VALUE_FOREACH(lentry, val)
			if (!li_value_list_has_len(lentry, 2)) return NULL;
			if (LI_VALUE_STRING != li_value_list_type_at(lentry, 0) && LI_VALUE_NONE != li_value_list_type_at(lentry, 0)) return NULL;
		LI_VALUE_END_FOREACH()
		return val;
	}
	return NULL;
}

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

static void _value_hash_free_key(gpointer data) {
	g_string_free((GString*) data, TRUE);
}

static void _value_hash_free_value(gpointer data) {
	li_value_free((liValue*) data);
}

GHashTable *li_value_new_hashtable(void) {
	return g_hash_table_new_full(
		(GHashFunc) g_string_hash, (GEqualFunc) g_string_equal,
		_value_hash_free_key, _value_hash_free_value);
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

liValue* li_common_value_copy_(liValue* val) {
	liValue *n;
	if (NULL == val) return NULL;

	switch (val->type) {
	case LI_VALUE_NONE: return li_value_new_none();
	case LI_VALUE_BOOLEAN: return li_value_new_bool(val->data.boolean);
	case LI_VALUE_NUMBER: return li_value_new_number(val->data.number);
	case LI_VALUE_STRING: return li_value_new_string(g_string_new_len(GSTR_LEN(val->data.string)));
	/* list: we have to copy every value in the list! */
	case LI_VALUE_LIST:
		n = li_value_new_list();
		g_ptr_array_set_size(n->data.list, val->data.list->len);
		for (guint i = 0; i < val->data.list->len; i++) {
			g_ptr_array_index(n->data.list, i) = li_value_copy(g_ptr_array_index(val->data.list, i));
		}
		return n;
	}
	return NULL;
}

static void _li_value_clear(liValue *val) {
	memset(val, 0, sizeof(*val));
	val->type = LI_VALUE_NONE;
}

void li_common_value_clear_(liValue *val) {
	if (NULL == val) return;

	switch (val->type) {
	case LI_VALUE_NONE:
	case LI_VALUE_BOOLEAN:
	case LI_VALUE_NUMBER:
		/* Nothing to free */
		break;
	case LI_VALUE_STRING:
		g_string_free(val->data.string, TRUE);
		break;
	case LI_VALUE_LIST:
		li_value_list_free(val->data.list);
		break;
	}
	_li_value_clear(val);
}

void li_value_free(liValue* val) {
	if (NULL == val) return;
	li_value_clear(val);
	g_slice_free(liValue, val);
}

void li_value_move(liValue *dest, liValue *src) {
	LI_FORCE_ASSERT(NULL != dest && NULL != src && dest != src);
	li_value_clear(dest);
	*dest = *src;
	_li_value_clear(src);
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

void li_value_list_free(GPtrArray *vallist) {
	if (NULL == vallist) return;
	for (gsize i = 0; i < vallist->len; i++) {
		li_value_free(g_ptr_array_index(vallist, i));
	}
	g_ptr_array_free(vallist, TRUE);
}

GString *li_common_value_to_string_(liValue *val) {
	GString *str = NULL;

	switch (val->type) {
	case LI_VALUE_NONE:
		str = g_string_new("null");
	case LI_VALUE_BOOLEAN:
		str = g_string_new(val->data.boolean ? "true" : "false");
		break;
	case LI_VALUE_NUMBER:
		str = g_string_sized_new(0);
		g_string_printf(str, "%" G_GINT64_FORMAT, val->data.number);
		break;
	case LI_VALUE_STRING:
		str = g_string_new_len(CONST_STR_LEN("\""));
		g_string_append_len(str, GSTR_LEN(val->data.string));
		g_string_append_c(str, '"');
		break;
	case LI_VALUE_LIST:
		str = g_string_new_len(CONST_STR_LEN("("));
		if (val->data.list->len) {
			GString *tmp = li_value_to_string(g_ptr_array_index(val->data.list, 0));
			g_string_append(str, tmp->str);
			g_string_free(tmp, TRUE);
			for (guint i = 1; i < val->data.list->len; i++) {
				tmp = li_value_to_string(g_ptr_array_index(val->data.list, i));
				g_string_append_len(str, CONST_STR_LEN(", "));
				g_string_append(str, tmp->str);
				g_string_free(tmp, TRUE);
			}
		}
		g_string_append_c(str, ')');
		break;
	}

	return str;
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

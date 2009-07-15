#include <lighttpd/angel_base.h>

liValue* li_value_new_none() {
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

liValue* li_value_new_range(liValueRange val) {
	liValue *v = g_slice_new0(liValue);
	v->data.range = val;
	v->type = LI_VALUE_RANGE;
	return v;
}

liValue* li_value_new_list() {
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

liValue* li_value_new_hash() {
	liValue *v = g_slice_new0(liValue);
	v->data.hash = g_hash_table_new_full(
		(GHashFunc) g_string_hash, (GEqualFunc) g_string_equal,
		_value_hash_free_key, _value_hash_free_value);
	v->type = LI_VALUE_HASH;
	return v;
}

liValue* li_value_copy(liValue* val) {
	liValue *n;

	switch (val->type) {
	case LI_VALUE_NONE: n = li_value_new_bool(FALSE); n->type = LI_VALUE_NONE; return n; /* hack */
	case LI_VALUE_BOOLEAN: return li_value_new_bool(val->data.boolean);
	case LI_VALUE_NUMBER: return li_value_new_number(val->data.number);
	case LI_VALUE_STRING: return li_value_new_string(g_string_new_len(GSTR_LEN(val->data.string)));
	case LI_VALUE_RANGE: return li_value_new_range(val->data.range);
	/* list: we have to copy every value in the list! */
	case LI_VALUE_LIST:
		n = li_value_new_list();
		g_ptr_array_set_size(n->data.list, val->data.list->len);
		for (guint i = 0; i < val->data.list->len; i++) {
			g_ptr_array_index(n->data.list, i) = li_value_copy(g_ptr_array_index(val->data.list, i));
		}
		return n;
	/* hash: iterate over hashtable, clone each value */
	case LI_VALUE_HASH:
		n = li_value_new_hash();
		{
			GHashTableIter iter;
			gpointer k, v;
			g_hash_table_iter_init(&iter, val->data.hash);
			while (g_hash_table_iter_next(&iter, &k, &v))
				g_hash_table_insert(n->data.hash, g_string_new_len(GSTR_LEN((GString*)k)), li_value_copy((liValue*)v));
		}
		return n;
	}
	return NULL;
}

static void li_value_list_free(GPtrArray *vallist) {
	if (!vallist) return;
	for (gsize i = 0; i < vallist->len; i++) {
		li_value_free(g_ptr_array_index(vallist, i));
	}
	g_ptr_array_free(vallist, TRUE);
}

void li_value_free(liValue* val) {
	if (!val) return;

	switch (val->type) {
	case LI_VALUE_NONE:
	case LI_VALUE_BOOLEAN:
	case LI_VALUE_NUMBER:
		/* Nothing to free */
		break;
	case LI_VALUE_STRING:
		g_string_free(val->data.string, TRUE);
		break;
	case LI_VALUE_RANGE:
		break;
	case LI_VALUE_LIST:
		li_value_list_free(val->data.list);
		break;
	case LI_VALUE_HASH:
		g_hash_table_destroy(val->data.hash);
		break;
	}
	val->type = LI_VALUE_NONE;
	g_slice_free(liValue, val);
}

const char* li_value_type_string(liValueType type) {
	switch(type) {
	case LI_VALUE_NONE:
		return "none";
	case LI_VALUE_BOOLEAN:
		return "boolean";
	case LI_VALUE_NUMBER:
		return "number";
	case LI_VALUE_STRING:
		return "string";
	case LI_VALUE_RANGE:
		return "range";
	case LI_VALUE_LIST:
		return "list";
	case LI_VALUE_HASH:
		return "hash";
	}
	return "<unknown>";
}

GString *li_value_to_string(liValue *val) {
	GString *str = NULL;

	switch (val->type) {
		case LI_VALUE_NONE:
			return NULL;
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
		case LI_VALUE_RANGE:
			str = g_string_sized_new(0);
			g_string_printf(str, "%" G_GINT64_FORMAT "-%" G_GINT64_FORMAT, val->data.range.from, val->data.range.to);
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
		case LI_VALUE_HASH:
		{
			GHashTableIter iter;
			gpointer k, v;
			GString *tmp;
			guint i = 0;

			str = g_string_new_len(CONST_STR_LEN("["));

			g_hash_table_iter_init(&iter, val->data.hash);
			while (g_hash_table_iter_next(&iter, &k, &v)) {
				if (i)
					g_string_append_len(str, CONST_STR_LEN(", "));
				tmp = li_value_to_string((liValue*)v);
				g_string_append_len(str, GSTR_LEN((GString*)k));
				g_string_append_len(str, CONST_STR_LEN(": "));
				g_string_append_len(str, GSTR_LEN(tmp));
				g_string_free(tmp, TRUE);
				i++;
			}

			g_string_append_c(str, ']');
			break;
		}
	}

	return str;
}

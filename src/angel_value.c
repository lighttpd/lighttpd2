#include <lighttpd/angel_base.h>

value* value_new_none() {
	value *v = g_slice_new0(value);
	v->type = VALUE_NONE;
	return v;
}

value* value_new_bool(gboolean val) {
	value *v = g_slice_new0(value);
	v->data.boolean = val;
	v->type = VALUE_BOOLEAN;
	return v;
}

value* value_new_number(gint64 val) {
	value *v = g_slice_new0(value);
	v->data.number = val;
	v->type = VALUE_NUMBER;
	return v;
}

value* value_new_string(GString *val) {
	value *v = g_slice_new0(value);
	v->data.string = val;
	v->type = VALUE_STRING;
	return v;
}

value* value_new_range(value_range val) {
	value *v = g_slice_new0(value);
	v->data.range = val;
	v->type = VALUE_RANGE;
	return v;
}

value* value_new_list() {
	value *v = g_slice_new0(value);
	v->data.list = g_ptr_array_new();
	v->type = VALUE_LIST;
	return v;
}

static void _value_hash_free_key(gpointer data) {
	g_string_free((GString*) data, TRUE);
}

static void _value_hash_free_value(gpointer data) {
	value_free((value*) data);
}

value* value_new_hash() {
	value *v = g_slice_new0(value);
	v->data.hash = g_hash_table_new_full(
		(GHashFunc) g_string_hash, (GEqualFunc) g_string_equal,
		_value_hash_free_key, _value_hash_free_value);
	v->type = VALUE_HASH;
	return v;
}

value* value_copy(value* val) {
	value *n;

	switch (val->type) {
	case VALUE_NONE: n = value_new_bool(FALSE); n->type = VALUE_NONE; return n; /* hack */
	case VALUE_BOOLEAN: return value_new_bool(val->data.boolean);
	case VALUE_NUMBER: return value_new_number(val->data.number);
	case VALUE_STRING: return value_new_string(g_string_new_len(GSTR_LEN(val->data.string)));
	case VALUE_RANGE: return value_new_range(val->data.range);
	/* list: we have to copy every value in the list! */
	case VALUE_LIST:
		n = value_new_list();
		g_ptr_array_set_size(n->data.list, val->data.list->len);
		for (guint i = 0; i < val->data.list->len; i++) {
			g_ptr_array_index(n->data.list, i) = value_copy(g_ptr_array_index(val->data.list, i));
		}
		return n;
	/* hash: iterate over hashtable, clone each value */
	case VALUE_HASH:
		n = value_new_hash();
		{
			GHashTableIter iter;
			gpointer k, v;
			g_hash_table_iter_init(&iter, val->data.hash);
			while (g_hash_table_iter_next(&iter, &k, &v))
				g_hash_table_insert(n->data.hash, g_string_new_len(GSTR_LEN((GString*)k)), value_copy((value*)v));
		}
		return n;
	}
	return NULL;
}

static void value_list_free(GPtrArray *vallist) {
	if (!vallist) return;
	for (gsize i = 0; i < vallist->len; i++) {
		value_free(g_ptr_array_index(vallist, i));
	}
	g_ptr_array_free(vallist, TRUE);
}

void value_free(value* val) {
	if (!val) return;

	switch (val->type) {
	case VALUE_NONE:
	case VALUE_BOOLEAN:
	case VALUE_NUMBER:
		/* Nothing to free */
		break;
	case VALUE_STRING:
		g_string_free(val->data.string, TRUE);
		break;
	case VALUE_RANGE:
		break;
	case VALUE_LIST:
		value_list_free(val->data.list);
		break;
	case VALUE_HASH:
		g_hash_table_destroy(val->data.hash);
		break;
	}
	val->type = VALUE_NONE;
	g_slice_free(value, val);
}

const char* value_type_string(value_type type) {
	switch(type) {
	case VALUE_NONE:
		return "none";
	case VALUE_BOOLEAN:
		return "boolean";
	case VALUE_NUMBER:
		return "number";
	case VALUE_STRING:
		return "string";
	case VALUE_RANGE:
		return "range";
	case VALUE_LIST:
		return "list";
	case VALUE_HASH:
		return "hash";
	}
	return "<unknown>";
}

GString *value_to_string(value *val) {
	GString *str = NULL;

	switch (val->type) {
		case VALUE_NONE:
			return NULL;
		case VALUE_BOOLEAN:
			str = g_string_new(val->data.boolean ? "true" : "false");
			break;
		case VALUE_NUMBER:
			str = g_string_sized_new(0);
			g_string_printf(str, "%" G_GINT64_FORMAT, val->data.number);
			break;
		case VALUE_STRING:
			str = g_string_new_len(CONST_STR_LEN("\""));
			g_string_append_len(str, GSTR_LEN(val->data.string));
			g_string_append_c(str, '"');
			break;
		case VALUE_RANGE:
			str = g_string_sized_new(0);
			g_string_printf(str, "%" G_GINT64_FORMAT "-%" G_GINT64_FORMAT, val->data.range.from, val->data.range.to);
			break;
		case VALUE_LIST:
			str = g_string_new_len(CONST_STR_LEN("("));
			if (val->data.list->len) {
				GString *tmp = value_to_string(g_ptr_array_index(val->data.list, 0));
				g_string_append(str, tmp->str);
				g_string_free(tmp, TRUE);
				for (guint i = 1; i < val->data.list->len; i++) {
					tmp = value_to_string(g_ptr_array_index(val->data.list, i));
					g_string_append_len(str, CONST_STR_LEN(", "));
					g_string_append(str, tmp->str);
					g_string_free(tmp, TRUE);
				}
			}
			g_string_append_c(str, ')');
			break;
		case VALUE_HASH:
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
				tmp = value_to_string((value*)v);
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

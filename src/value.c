#include "base.h"

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

value* value_new_list() {
	value *v = g_slice_new0(value);
	v->data.list = g_array_new(FALSE, TRUE, sizeof(value*));
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

value* value_new_action(server *srv, action *a) {
	value *v = g_slice_new0(value);
	v->data.val_action.srv = srv;
	v->data.val_action.action = a;
	v->type = VALUE_ACTION;
	return v;
}

value* value_new_condition(server *srv, condition *c) {
	value *v = g_slice_new0(value);
	v->data.val_cond.srv = srv;
	v->data.val_cond.cond = c;
	v->type = VALUE_CONDITION;
	return v;
}

value* value_copy(value* val) {
	value *n;

	switch (val->type) {
	case VALUE_NONE: n = value_new_bool(FALSE); n->type = VALUE_NONE; return n; /* hack */
	case VALUE_BOOLEAN: return value_new_bool(val->data.boolean);
	case VALUE_NUMBER: return value_new_number(val->data.number);
	case VALUE_STRING: return value_new_string(g_string_new_len(GSTR_LEN(val->data.string)));
	/* list: we have to copy every value in the list! */
	case VALUE_LIST:
		n = value_new_list();
		g_array_set_size(n->data.list, val->data.list->len);
		for (guint i = 0; i < val->data.list->len; i++) {
			g_array_index(n->data.list, value*, i) = value_copy(g_array_index(val->data.list, value*, i));
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
	case VALUE_ACTION:
		action_acquire(val->data.val_action.action);
		n = value_new_action(val->data.val_action.srv, val->data.val_action.action);
		return n;
	case VALUE_CONDITION:
		condition_acquire(val->data.val_cond.cond);
		n = value_new_condition(val->data.val_cond.srv, val->data.val_cond.cond);
		return n;
	}
	return NULL;
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
	case VALUE_LIST:
		value_list_free(val->data.list);
		break;
	case VALUE_HASH:
		g_hash_table_destroy(val->data.hash);
		break;
	case VALUE_ACTION:
		action_release(val->data.val_action.srv, val->data.val_action.action);
		break;
	case VALUE_CONDITION:
		condition_release(val->data.val_cond.srv, val->data.val_cond.cond);
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
	case VALUE_LIST:
		return "list";
	case VALUE_HASH:
		return "hash";
	case VALUE_ACTION:
		return "action";
	case VALUE_CONDITION:
		return "condition";
	}
	return "<unknown>";
}

void value_list_free(GArray *vallist) {
	if (!vallist) return;
	for (gsize i = 0; i < vallist->len; i++) {
		value_free(g_array_index(vallist, value*, i));
	}
	g_array_free(vallist, TRUE);
}

GString *value_to_string(value *val) {
	GString *str;

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
		case VALUE_LIST:
			str = g_string_new_len(CONST_STR_LEN("("));
			if (val->data.list->len) {
				GString *tmp = value_to_string(g_array_index(val->data.list, value*, 0));
				g_string_append(str, tmp->str);
				g_string_free(tmp, TRUE);
				for (guint i = 1; i < val->data.list->len; i++) {
					tmp = value_to_string(g_array_index(val->data.list, value*, i));
					g_string_append_len(str, CONST_STR_LEN(", "));
					g_string_append(str, tmp->str);
					g_string_free(tmp, TRUE);
				}
			}
			g_string_append_c(str, ')');
			break;
		case VALUE_HASH:
		{
			str = g_string_new_len(CONST_STR_LEN("["));
			GHashTableIter iter;
			gpointer k, v;
			GString *tmp;
			guint i = 0;


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
		case VALUE_ACTION:
			str = g_string_new_len(CONST_STR_LEN("<action>"));
			break;
		case VALUE_CONDITION:
			str = g_string_new_len(CONST_STR_LEN("<condition>"));
			break;
		default:
			return NULL;
	}

	return str;
}

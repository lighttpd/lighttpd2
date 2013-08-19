#include <lighttpd/base.h>

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
	v->data.list = g_array_new(FALSE, TRUE, sizeof(liValue*));
	v->type = LI_VALUE_LIST;
	return v;
}

static void _value_hash_free_key(gpointer data) {
	g_string_free((GString*) data, TRUE);
}

static void _value_hash_free_value(gpointer data) {
	li_value_free((liValue*) data);
}

liValue* li_value_new_hash(void) {
	liValue *v = g_slice_new0(liValue);
	v->data.hash = li_value_new_hashtable();
	v->type = LI_VALUE_HASH;
	return v;
}

liValue* li_value_new_action(liServer *srv, liAction *a) {
	liValue *v = g_slice_new0(liValue);
	v->data.val_action.srv = srv;
	v->data.val_action.action = a;
	v->type = LI_VALUE_ACTION;
	return v;
}

liValue* li_value_new_condition(liServer *srv, liCondition *c) {
	liValue *v = g_slice_new0(liValue);
	v->data.val_cond.srv = srv;
	v->data.val_cond.cond = c;
	v->type = LI_VALUE_CONDITION;
	return v;
}

GHashTable *li_value_new_hashtable(void) {
	return g_hash_table_new_full(
		(GHashFunc) g_string_hash, (GEqualFunc) g_string_equal,
		_value_hash_free_key, _value_hash_free_value);
}

void li_value_list_append(liValue *list, liValue *item) {
	assert(LI_VALUE_LIST == list->type);
	g_array_append_val(list->data.list, item);
}

void li_value_wrap_in_list(liValue *val) {
	liValue *item = li_value_extract(val);
	val->type = LI_VALUE_LIST;
	val->data.list = g_array_new(FALSE, TRUE, sizeof(liValue*));
	g_array_append_val(val->data.list, item);
}

liValue* li_value_copy(liValue* val) {
	liValue *n;

	switch (val->type) {
	case LI_VALUE_NONE: return li_value_new_none();
	case LI_VALUE_BOOLEAN: return li_value_new_bool(val->data.boolean);
	case LI_VALUE_NUMBER: return li_value_new_number(val->data.number);
	case LI_VALUE_STRING: return li_value_new_string(g_string_new_len(GSTR_LEN(val->data.string)));
	/* list: we have to copy every value in the list! */
	case LI_VALUE_LIST:
		n = li_value_new_list();
		g_array_set_size(n->data.list, val->data.list->len);
		for (guint i = 0; i < val->data.list->len; i++) {
			g_array_index(n->data.list, liValue*, i) = li_value_copy(g_array_index(val->data.list, liValue*, i));
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
	case LI_VALUE_ACTION:
		li_action_acquire(val->data.val_action.action);
		n = li_value_new_action(val->data.val_action.srv, val->data.val_action.action);
		return n;
	case LI_VALUE_CONDITION:
		li_condition_acquire(val->data.val_cond.cond);
		n = li_value_new_condition(val->data.val_cond.srv, val->data.val_cond.cond);
		return n;
	}
	return NULL;
}

static void _li_value_clear(liValue *val) {
	memset(val, 0, sizeof(*val));
	val->type = LI_VALUE_NONE;
}

void li_value_clear(liValue *val) {
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
	case LI_VALUE_HASH:
		g_hash_table_destroy(val->data.hash);
		break;
	case LI_VALUE_ACTION:
		li_action_release(val->data.val_action.srv, val->data.val_action.action);
		break;
	case LI_VALUE_CONDITION:
		li_condition_release(val->data.val_cond.srv, val->data.val_cond.cond);
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
	assert(NULL != dest && NULL != src && dest != src);
	li_value_clear(dest);
	*dest = *src;
	_li_value_clear(src);
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
	case LI_VALUE_LIST:
		return "list";
	case LI_VALUE_HASH:
		return "hash";
	case LI_VALUE_ACTION:
		return "action";
	case LI_VALUE_CONDITION:
		return "condition";
	}
	return "<unknown>";
}

void li_value_list_free(GArray *vallist) {
	if (!vallist) return;
	for (gsize i = 0; i < vallist->len; i++) {
		li_value_free(g_array_index(vallist, liValue*, i));
	}
	g_array_free(vallist, TRUE);
}

GString *li_value_to_string(liValue *val) {
	GString *str;

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
		case LI_VALUE_LIST:
			str = g_string_new_len(CONST_STR_LEN("("));
			if (val->data.list->len) {
				GString *tmp = li_value_to_string(g_array_index(val->data.list, liValue*, 0));
				g_string_append(str, tmp->str);
				g_string_free(tmp, TRUE);
				for (guint i = 1; i < val->data.list->len; i++) {
					tmp = li_value_to_string(g_array_index(val->data.list, liValue*, i));
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
		case LI_VALUE_ACTION:
			str = g_string_new_len(CONST_STR_LEN("<action>"));
			break;
		case LI_VALUE_CONDITION:
			str = g_string_new_len(CONST_STR_LEN("<condition>"));
			break;
		default:
			return NULL;
	}

	return str;
}

gpointer li_value_extract_ptr(liValue *val) {
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
	case LI_VALUE_HASH:
		ptr =  val->data.hash;
		break;
	case LI_VALUE_ACTION:
		ptr = val->data.val_action.action;
		break;
	case LI_VALUE_CONDITION:
		ptr = val->data.val_action.action;
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

GArray* li_value_extract_list(liValue *val) {
	GArray* result;
	if (NULL == val || val->type != LI_VALUE_LIST) return NULL;
	result = val->data.list;
	_li_value_clear(val);
	return result;
}

GHashTable* li_value_extract_hash(liValue *val) {
	GHashTable* result;
	if (NULL == val || val->type != LI_VALUE_HASH) return NULL;
	result = val->data.hash;
	_li_value_clear(val);
	return result;
}

liAction* li_value_extract_action(liValue *val) {
	liAction* result;
	if (NULL == val || val->type != LI_VALUE_ACTION) return NULL;
	result = val->data.val_action.action;
	_li_value_clear(val);
	return result;
}

liCondition* li_value_extract_condition(liValue *val) {
	liCondition* result;
	if (NULL == val || val->type != LI_VALUE_CONDITION) return NULL;
	result = val->data.val_cond.cond;
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

	if (val->type == LI_VALUE_HASH) {
		GHashTable *table = li_value_extract_hash(val);
		GArray *list;

		GHashTableIter hti;
		gpointer hkey, hvalue;

		{
			/* convert val to list */
			liValue *vlist = li_value_new_list();
			li_value_move(val, vlist);
			li_value_free(vlist);
		}
		list = val->data.list;

		g_hash_table_iter_init(&hti, table);
		while (g_hash_table_iter_next(&hti, &hkey, &hvalue)) {
			GString *htkey = hkey; liValue *htval = hvalue;
			liValue *hkeyval = li_value_new_string(htkey);
			liValue *pair = li_value_new_list();
			g_array_append_val(pair->data.list, hkeyval);
			g_array_append_val(pair->data.list, htval);
			g_array_append_val(list, pair);
		}
		g_hash_table_steal_all(table); /* content was moved to list */
		g_hash_table_destroy(table);

		return val;
	} else if (val->type == LI_VALUE_LIST) {
		/* verify key-value list properties */
		GArray *list = val->data.list;
		guint i;
		for (i = 0; i < list->len; ++i) {
			liValue *lentry = g_array_index(list, liValue*, i);
			GArray *entrylist;
			liValue *key;

			if (lentry->type != LI_VALUE_LIST) return NULL;
			entrylist = lentry->data.list;
			if (2 != entrylist->len) return NULL;
			key = g_array_index(entrylist, liValue*, 0);
			if (key->type != LI_VALUE_STRING && key->type != LI_VALUE_NONE) return NULL;
		}
		return val;
	}
	return NULL;
}

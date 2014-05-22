/* include from angel / main;
 * as some "common" functions need to call the actual implementations
 * in angel / main they cannot live in "common"
 */

static void _li_value_clear(liValue *val) {
	memset(val, 0, sizeof(*val));
	val->type = LI_VALUE_NONE;
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
	default:
		/* other cases need to be handled by li_value_copy */
		break;
	}
	return NULL;
}

GString *li_common_value_to_string_(liValue *val) {
	GString *str = NULL;

	switch (val->type) {
	case LI_VALUE_NONE:
		str = g_string_new("null");
		break;
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
	default:
		/* other cases need to be handled by li_value_to_string */
		break;
	}

	return str;
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

void li_value_list_free(GPtrArray *vallist) {
	if (NULL == vallist) return;
	for (gsize i = 0; i < vallist->len; i++) {
		li_value_free(g_ptr_array_index(vallist, i));
	}
	g_ptr_array_free(vallist, TRUE);
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
	default:
		/* other cases need to be handled by li_value_clear */
		break;
	}
	_li_value_clear(val);
}

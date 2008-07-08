
#include "options.h"

void options_init()
{
	options = g_array_new(FALSE, TRUE, sizeof(option *));
	options_hash = g_hash_table_new((GHashFunc) g_int_hash, (GEqualFunc) g_int_equal);
}

option* option_new_bool(gboolean val) {
	option *opt = g_slice_new0(option);
	opt->value.opt_bool = val;
	return opt;
}

option* option_new_int(gint val) {
	option *opt = g_slice_new0(option);
	opt->value.opt_int = val;
	return opt;
}

option* option_new_string(GString *val) {
	option *opt = g_slice_new0(option);
	opt->value.opt_string = val;
	return opt;
}

option* option_new_list() {
	option *opt = g_slice_new0(option);
	opt->value.opt_list = g_array_new(FALSE, TRUE, sizeof(option*));
	return opt;
}

void _option_hash_free_key(gpointer data) {
	g_string_free((GString*) data, TRUE);
}

void _option_hash_free_value(gpointer data) {
	option_free((option*) data);
}

option* option_new_hash() {
	option *opt = g_slice_new0(option);
	opt->value.opt_hash = g_hash_table_new_full(
		(GHashFunc) g_string_hash, (GEqualFunc) g_string_equal,
		_option_hash_free_key, _option_hash_free_value);
	return opt;
}


void option_free(option* opt) {
	if (!opt) return;

	switch (opt->type) {
	case OPTION_NONE:
	case OPTION_BOOLEAN:
	case OPTION_INT:
		/* Nothing to free */
		break;
	case OPTION_STRING:
		g_string_free(opt->value.opt_string, TRUE);
		break;
	case OPTION_LIST:
		option_list_free(opt->value.opt_list);
		break;
	case OPTION_HASH:
		g_hash_table_destroy((GHashTable*) opt->value.opt_hash);
		break;
	}
	opt->type = OPTION_NONE;
	g_slice_free(option, opt);
}

const char* option_type_string(option_type type) {
	switch(type) {
	case OPTION_NONE:
		return "none";
	case OPTION_BOOLEAN:
		return "boolean";
	case OPTION_INT:
		return "int";
	case OPTION_STRING:
		return "string";
	case OPTION_LIST:
		return "list";
	case OPTION_HASH:
		return "hash";
	}
	return "<unknown>";
}

void option_list_free(GArray *optlist) {
	if (!optlist) return;
	for (gsize i = 0; i < optlist->len; i++) {
		option_free(g_array_index(optlist, option*, i));
	}
	g_array_free(optlist, TRUE);
}

/* Extract value from option, destroy option */
gpointer option_extract_value(option *opt) {
	gpointer val = NULL;
	if (!opt) return NULL;

	switch (opt->type) {
		case OPTION_NONE:
			break;
		case OPTION_BOOLEAN:
			val = GINT_TO_POINTER(opt->value.opt_bool);
			break;
		case OPTION_INT:
			val =  GINT_TO_POINTER(opt->value.opt_int);
			break;
		case OPTION_STRING:
			val =  opt->value.opt_string;
			break;
		case OPTION_LIST:
			val =  opt->value.opt_list;
			break;
		case OPTION_HASH:
			val =  opt->value.opt_hash;
			break;
	}
	opt->type = OPTION_NONE;
	g_slice_free(option, opt);
	return val;
}



gboolean option_register(GString *name, option *opt) {
	guint *ndx;

	ndx = g_slice_new(guint);

	/* check if not already registered */
	if (option_index(name, ndx))
		return FALSE;

	g_array_append_val(options, opt);
	*ndx = options->len;
	g_hash_table_insert(options_hash, (gpointer) name, (gpointer) ndx);

	return TRUE;
}


gboolean option_unregister(GString *name) {
	UNUSED(name);
	assert(NULL == "does this even make sense?");
}


gboolean option_index(GString *name, guint *ndx) {
	guint *val;

	val = (guint *) g_hash_table_lookup(options_hash, (gconstpointer) name);

	if (val == NULL)
		return FALSE;

	*ndx = *val;
	return TRUE;
}


#include "base.h"
#include "options.h"

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

gboolean option_get_index(server *srv, GString *name, gsize *ndx)
{
	gpointer ptr;

	ptr = g_hash_table_lookup(srv->options, (gconstpointer) name);

	if (ptr == NULL)
		return FALSE;

	*ndx = 0;

	return TRUE;
}

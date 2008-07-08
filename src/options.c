
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
	guint i;

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
			for (i=0; i<opt->value.opt_list->len; i++)
				option_free(g_array_index(opt->value.opt_list, option *, i));
			g_array_free(opt->value.opt_list, FALSE);
			break;
		case OPTION_HASH:
			g_hash_table_destroy(opt->value.opt_hash);
			break;
	}
	opt->type = OPTION_NONE;
	g_slice_free(option, opt);
}


#include "plugin.h"
#include "log.h"

static server_option* find_option(server *srv, const char *key) {
	return (server_option*) g_hash_table_lookup(srv->options, key);
}

gboolean parse_option(server *srv, const char *key, option *opt, option_set *mark) {
	server_option *sopt;

	if (!srv || !key || !mark) return FALSE;

	sopt = find_option(srv, key);
	if (!sopt) {
		ERROR(srv, "Unknown option '%s'", key);
		return FALSE;
	}

	if (sopt->type != opt->type) {
		ERROR(srv, "Unexpected option type '%s', expected '%s'",
			option_type_string(opt->type), option_type_string(sopt->type));
		return FALSE;
	}

	if (!sopt->parse_option) {
		mark->value = option_extract_value(opt);
	} else {
		if (!sopt->parse_option(srv, sopt->p->data, sopt->module_index, opt, &mark->value)) {
			/* errors should be logged by parse function */
			return FALSE;
		}
	}

	mark->ndx = sopt->index;
	mark->sopt = sopt;

	return TRUE;
}

void release_option(server *srv, option_set *mark) { /** Does not free the option_set memory */
	server_option *sopt = mark->sopt;
	if (!srv || !mark || !sopt) return;

	mark->sopt = NULL;
	if (!sopt->free_option) {
		switch (sopt->type) {
		case OPTION_NONE:
		case OPTION_BOOLEAN:
		case OPTION_INT:
			/* Nothing to free */
			break;
		case OPTION_STRING:
			g_string_free((GString*) mark->value, TRUE);
			break;
		case OPTION_LIST:
			option_list_free((GArray*) mark->value);
			break;
		case OPTION_HASH:
			g_hash_table_destroy((GHashTable*) mark->value);
			break;
		}
	} else {
		sopt->free_option(srv, sopt->p->data, sopt->module_index, mark->value);
	}
	mark->value = NULL;
}

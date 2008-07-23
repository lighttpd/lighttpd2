
#include "plugin.h"
#include "log.h"

static plugin* plugin_new(const gchar *name) {
	plugin *p = g_slice_new0(plugin);
	p->name = name;
	return p;
}

void plugin_free(plugin *p) {
	if (!p) return;

	g_slice_free(plugin, p);
}


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

gboolean plugin_register(server *srv, const gchar *name, PluginInit init) {
	plugin *p;

	if (!init) {
		ERROR(srv, "Module '%s' needs an init function", name);
		return FALSE;
	}

	if (g_hash_table_lookup(srv->plugins, name)) {
		ERROR(srv, "Module '%s' already registered", name);
		return FALSE;
	}

	p = plugin_new(name);
	g_hash_table_insert(srv->plugins, (gchar*) p->name, p);

	init(srv, p);

	if (p->options) {
		size_t i;
		server_option *so;
		const plugin_option *po;

		for (i = 0; (po = &p->options[i])->key; i++) {
			if (NULL != (so = (server_option*)g_hash_table_lookup(srv->options, po->key))) {
				ERROR(srv, "Option '%s' already registered by plugin '%s', unloading '%s'",
					po->key,
					so->p ? so->p->name : "<none>",
					p->name);
				break;
			}
			so = g_slice_new0(server_option);
			so->type = po->type;
			so->parse_option = po->parse_option;
			so->free_option = po->free_option;
			so->index = g_hash_table_size(srv->options);
			so->module_index = i;
			so->p = p;
			g_hash_table_insert(srv->options, (gchar*) po->key, so);
		}

		if (po->key) {
			while (i-- > 0) {
				po = &p->options[i];
				g_slice_free(server_option, g_hash_table_lookup(srv->options, po->key));
				g_hash_table_remove(srv->options, po->key);
			}
			g_hash_table_remove(srv->plugins, p->name);
			if (p->free) p->free(srv, p);
			plugin_free(p);
			return FALSE;
		}
	}

	return TRUE;
}

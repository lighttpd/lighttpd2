
#include "base.h"

static action_result core_handle_static(server *srv, connection *con, gpointer param) {
	UNUSED(param);
	/* TODO */
	CON_ERROR(srv, con, "%s", "Not implemented yet");
	return HANDLER_ERROR;
}

static gboolean core_static(server *srv, gpointer p_d, option *opt, action_func *func) {
	UNUSED(p_d);
	if (opt) {
		ERROR(srv, "%s", "static action doesn't have parameters");
		return FALSE;
	}

	func->func = core_handle_static;
	func->free = NULL;
	func->param = NULL;
	return TRUE;
}

static gboolean core_listen(server *srv, gpointer p_d, option *opt) {
	UNUSED(p_d);
	if (opt->type != OPTION_STRING) {
		ERROR(srv, "%s", "listen expects a string as parameter");
		return FALSE;
	}

	TRACE(srv, "will listen to '%s'", opt->value.opt_string->str);
	return TRUE;
}

static const plugin_option options[] = {
	{ "static-file.exclude", OPTION_LIST, NULL, NULL },
	{ NULL, 0, NULL, NULL }
};

static const plugin_action actions[] = {
	{ "static", core_static },
	{ NULL, NULL }
};

static const plugin_setup setups[] = {
	{ "listen", core_listen },
	{ NULL, NULL }
};

void plugin_core_init(server *srv, plugin *p) {
	UNUSED(srv);

	p->options = options;
	p->actions = actions;
	p->setups = setups;
}

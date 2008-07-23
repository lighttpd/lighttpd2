
#include "base.h"


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

static const plugin_setup setups[] = {
	{ "listen", core_listen },
	{ NULL, NULL }
};

void plugin_core_init(server *srv, plugin *p) {
	UNUSED(srv);

	p->options = options;
}

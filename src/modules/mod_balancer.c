
#include <lighttpd/base.h>

static const plugin_option options[] = {
	{ NULL, 0, NULL, NULL, NULL }
};

static const plugin_action actions[] = {
// 	{ "balancer.rr", status_page },
	{ NULL, NULL }
};

static const plugin_setup setups[] = {
	{ NULL, NULL }
};


static void plugin_init(server *srv, plugin *p) {
	UNUSED(srv);

	p->options = options;
	p->actions = actions;
	p->setups = setups;
}


LI_API gboolean mod_balancer_init(modules *mods, module *mod) {
	MODULE_VERSION_CHECK(mods);

	mod->config = plugin_register(mods->main, "mod_balancer", plugin_init);

	return mod->config != NULL;
}

LI_API gboolean mod_balancer_free(modules *mods, module *mod) {
	if (mod->config)
		plugin_free(mods->main, mod->config);

	return TRUE;
}

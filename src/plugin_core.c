
#include "base.h"

static action* core_list(server *srv, plugin* p, option *opt) {
	action *a;
	guint i;
	UNUSED(p);

	if (opt->type != OPTION_LIST) {
		ERROR(srv, "expected list, got %s", option_type_string(opt->type));
		return NULL;
	}

	a = action_new_list();
	for (i = 0; i < opt->value.opt_list->len; i++) {
		option *oa = g_array_index(opt->value.opt_list, option*, i);
		if (oa->type != OPTION_ACTION) {
			ERROR(srv, "expected action at entry %u of list, got %s", i, option_type_string(oa->type));
			action_release(srv, a);
			return NULL;
		}
		assert(srv == oa->value.opt_action.srv);
		action_acquire(oa->value.opt_action.action);
		g_array_append_val(a->value.list->actions, oa->value.opt_action.action);
	}
	option_free(opt);
	return a;
}

static action* core_when(server *srv, plugin* p, option *opt) {
	option *opt_cond, *opt_act;
	action *a;
	UNUSED(p);

	if (opt->type != OPTION_LIST) {
		ERROR(srv, "expected list, got %s", option_type_string(opt->type));
		return NULL;
	}
	if (opt->value.opt_list->len != 2) {
		ERROR(srv, "expected list with length 2, has length %u", opt->value.opt_list->len);
		return NULL;
	}
	opt_cond = g_array_index(opt->value.opt_list, option*, 0);
	opt_act = g_array_index(opt->value.opt_list, option*, 1);
	if (opt_act->type != OPTION_ACTION) {
		ERROR(srv, "expected action as second parameter, got %s", option_type_string(opt->type));
		return NULL;
	}
	if (opt_cond->type != OPTION_CONDITION) {
		ERROR(srv, "expected condition as first parameter, got %s", option_type_string(opt->type));
		return NULL;
	}
	a = action_new_condition(opt_cond->value.opt_cond.cond, action_list_from_action(opt_act->value.opt_action.action));
	option_free(opt);
	return a;
}

static action_result core_handle_static(server *srv, connection *con, gpointer param) {
	UNUSED(param);
	/* TODO */
	CON_ERROR(srv, con, "%s", "Not implemented yet");
	return HANDLER_ERROR;
}

static action* core_static(server *srv, plugin* p, option *opt) {
	UNUSED(p);
	if (opt) {
		ERROR(srv, "%s", "static action doesn't have parameters");
		return NULL;
	}

	return action_new_function(core_handle_static, NULL, NULL);
}

static gboolean core_listen(server *srv, plugin* p, option *opt) {
	UNUSED(p);
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
	{ "list", core_list },
	{ "when", core_when },
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

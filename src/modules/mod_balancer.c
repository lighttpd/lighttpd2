
#include <lighttpd/base.h>

LI_API gboolean mod_balancer_init(liModules *mods, liModule *mod);
LI_API gboolean mod_balancer_free(liModules *mods, liModule *mod);

typedef enum {
	BE_ALIVE,
	BE_OVERLOADED,
	BE_DOWN,
	BE_DOWN_RETRY
} backend_state;

typedef enum {
	BAL_ALIVE,
	BAL_OVERLOADED,
	BAL_DOWN
} balancer_state;

struct backend {
	liAction *act;
	guint load;
	backend_state state;
};
typedef struct backend backend;

struct balancer {
	GArray *backends;
	balancer_state state;
};
typedef struct balancer balancer;

static balancer* balancer_new() {
	balancer *b = g_slice_new(balancer);
	b->backends = g_array_new(FALSE, TRUE, sizeof(backend));

	return b;
}

static void balancer_free(liServer *srv, balancer *b) {
	guint i;
	if (!b) return;
	for (i = 0; i < b->backends->len; i++) {
		backend *be = &g_array_index(b->backends, backend, i);
		li_action_release(srv, be->act);
	}
	g_array_free(b->backends, TRUE);
	g_slice_free(balancer, b);
}

static gboolean balancer_fill_backends(balancer *b, liServer *srv, liValue *val) {
	if (val->type == LI_VALUE_ACTION) {
		backend be = { val->data.val_action.action, 0, BE_ALIVE };
		assert(srv == val->data.val_action.srv);
		li_action_acquire(be.act);
		g_array_append_val(b->backends, be);
		return TRUE;
	} else if (val->type == LI_VALUE_LIST) {
		guint i;
		if (val->data.list->len == 0) {
			ERROR(srv, "%s", "expected non-empty list");
			return FALSE;
		}
		for (i = 0; i < val->data.list->len; i++) {
			liValue *oa = g_array_index(val->data.list, liValue*, i);
			if (oa->type != LI_VALUE_ACTION) {
				ERROR(srv, "expected action at entry %u of list, got %s", i, li_value_type_string(oa->type));
				return FALSE;
			}
			assert(srv == oa->data.val_action.srv);
			{
				backend be = { oa->data.val_action.action, 0, BE_ALIVE };
				li_action_acquire(be.act);
				g_array_append_val(b->backends, be);
			}
		}
		return TRUE;
	} else {
		ERROR(srv, "expected list, got %s", li_value_type_string(val->type));
		return FALSE;
	}
}

static liHandlerResult balancer_act_select(liVRequest *vr, gboolean backlog_provided, gpointer param, gpointer *context) {
	balancer *b = (balancer*) param;
	gint be_ndx = 0;
	backend *be = &g_array_index(b->backends, backend, be_ndx);

	UNUSED(backlog_provided);

	/* TODO implement some selection algorithms */

	be->load++;
	li_action_enter(vr, be->act);
	*context = GINT_TO_POINTER(be_ndx);

	return LI_HANDLER_GO_ON;
}

static liHandlerResult balancer_act_fallback(liVRequest *vr, gboolean backlog_provided, gpointer param, gpointer *context, liBackendError error) {
	balancer *b = (balancer*) param;
	gint be_ndx = GPOINTER_TO_INT(*context);
	backend *be = &g_array_index(b->backends, backend, be_ndx);

	UNUSED(backlog_provided);
	UNUSED(error);

	if (be_ndx < 0) return LI_HANDLER_GO_ON;

	/* TODO implement fallback/backlog */

	be->load--;
	*context = GINT_TO_POINTER(-1);
	li_vrequest_backend_error(vr, error);
	return LI_HANDLER_GO_ON;
}

static liHandlerResult balancer_act_finished(liVRequest *vr, gpointer param, gpointer context) {
	balancer *b = (balancer*) param;
	gint be_ndx = GPOINTER_TO_INT(context);
	backend *be = &g_array_index(b->backends, backend, be_ndx);

	UNUSED(vr);

	if (be_ndx < 0) return LI_HANDLER_GO_ON;

	/* TODO implement backlog */

	be->load--;
	return LI_HANDLER_GO_ON;
}

static void balancer_act_free(liServer *srv, gpointer param) {
	balancer_free(srv, (balancer*) param);
}

static liAction* balancer_rr(liServer *srv, liPlugin* p, liValue *val) {
	balancer *b;
	liAction *a;
	UNUSED(p);

	if (!val) {
		ERROR(srv, "%s", "need parameter");
		return NULL;
	}

	b = balancer_new();
	if (!balancer_fill_backends(b, srv, val)) {
		balancer_free(srv, b);
		return NULL;
	}

	a = li_action_new_balancer(balancer_act_select, balancer_act_fallback, balancer_act_finished, balancer_act_free, b, TRUE);
	return a;
}


static const liPluginOption options[] = {
	{ NULL, 0, NULL, NULL, NULL }
};

static const liPluginAction actions[] = {
	{ "balancer.rr", balancer_rr },
	{ NULL, NULL }
};

static const liliPluginSetupCB setups[] = {
	{ NULL, NULL }
};


static void plugin_init(liServer *srv, liPlugin *p) {
	UNUSED(srv);

	p->options = options;
	p->actions = actions;
	p->setups = setups;
}


gboolean mod_balancer_init(liModules *mods, liModule *mod) {
	MODULE_VERSION_CHECK(mods);

	mod->config = li_plugin_register(mods->main, "mod_balancer", plugin_init);

	return mod->config != NULL;
}

gboolean mod_balancer_free(liModules *mods, liModule *mod) {
	if (mod->config)
		li_plugin_free(mods->main, mod->config);

	return TRUE;
}

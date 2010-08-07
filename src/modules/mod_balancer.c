/*
 * mod_balancer - balance between different backends
 *
 * Description:
 *     mod_balancer balances between different backends;
 *
 * Setups:
 *     none
 * Options:
 *     none
 * Actions:
 *     balance.rr <actions> - balance between actions (list or single action)
*
 * Example config:
 *     balance.rr { fastcgi "127.0.0.1:9090"; };
 *
 * Todo:
 *     - add some select function (current: always take first)
 *     - support backlogs
 *
 * Author:
 *     Copyright (c) 2009 Stefan Bühler
 */


#include <lighttpd/base.h>
#include <lighttpd/plugin_core.h>

LI_API gboolean mod_balancer_init(liModules *mods, liModule *mod);
LI_API gboolean mod_balancer_free(liModules *mods, liModule *mod);

typedef enum {
	BE_ALIVE,
	BE_OVERLOADED,
	BE_DOWN
} backend_state;

typedef enum {
	BAL_ALIVE,
	BAL_OVERLOADED,
	BAL_DOWN
} balancer_state;

typedef enum {
	BM_SQF,
	BM_ROUNDROBIN
} balancer_method;

struct backend {
	liAction *act;
	guint load;
	backend_state state;
	ev_tstamp wake;
};
typedef struct backend backend;

struct balancer {
	GMutex *lock;
	GArray *backends;
	balancer_state state;
	balancer_method method;
	gint next_ndx;

	liPlugin *p;
};
typedef struct balancer balancer;

static balancer* balancer_new(liPlugin *p, balancer_method method) {
	balancer *b = g_slice_new(balancer);
	b->lock = g_mutex_new();
	b->backends = g_array_new(FALSE, TRUE, sizeof(backend));
	b->method = method;
	b->state = BAL_ALIVE;
	b->next_ndx = 0;
	b->p = p;

	return b;
}

static void balancer_free(liServer *srv, balancer *b) {
	guint i;
	if (!b) return;
	g_mutex_free(b->lock);
	for (i = 0; i < b->backends->len; i++) {
		backend *be = &g_array_index(b->backends, backend, i);
		li_action_release(srv, be->act);
	}
	g_array_free(b->backends, TRUE);
	g_slice_free(balancer, b);
}

static gboolean balancer_fill_backends(balancer *b, liServer *srv, liValue *val) {
	if (val->type == LI_VALUE_ACTION) {
		backend be = { val->data.val_action.action, 0, BE_ALIVE, 0 };
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
				backend be = { oa->data.val_action.action, 0, BE_ALIVE, 0 };
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
	gint be_ndx, load;
	guint i, j;
	backend *be;
	ev_tstamp now = ev_now(vr->wrk->loop);
	gboolean all_dead = TRUE;
	gboolean debug = _OPTION(vr, b->p, 0).boolean;

	UNUSED(backlog_provided);

	be_ndx = -1;

	g_mutex_lock(b->lock);

	switch (b->method) {
	case BM_SQF:
		load = -1;

		for (i = 0; i < b->backends->len; i++) {
			be = &g_array_index(b->backends, backend, i);

			if (now >= be->wake) be->state = BE_ALIVE;
			if (be->state != BE_DOWN) all_dead = FALSE;
			if (be->state != BE_ALIVE) continue;

			if (load == -1 || load > (gint) be->load) {
				be_ndx = i;
				load = be->load;
			}
		}

		break;
	case BM_ROUNDROBIN:
		for (j = 0; j < b->backends->len; j++) {
			i = (b->next_ndx + j) % b->backends->len;
			be = &g_array_index(b->backends, backend, i);

			if (now >= be->wake) be->state = BE_ALIVE;
			if (be->state != BE_DOWN) all_dead = FALSE;
			if (be->state != BE_ALIVE) continue;

			be_ndx = i;
			break; /* use first alive backend */
		}

		break;
	}

	if (-1 == be_ndx) {
		/* Couldn't find a active backend */
		if (all_dead) {
			li_vrequest_backend_dead(vr);
		} else {
			li_vrequest_backend_overloaded(vr);
		}

		g_mutex_unlock(b->lock);

		return LI_HANDLER_GO_ON;
	}

	be = &g_array_index(b->backends, backend, be_ndx);
	be->load++;
	b->next_ndx = be_ndx + 1;

	g_mutex_unlock(b->lock);

	if (debug || CORE_OPTION(LI_CORE_OPTION_DEBUG_REQUEST_HANDLING).boolean){
		VR_DEBUG(vr, "balancer select: %i", be_ndx);
	}

	li_action_enter(vr, be->act);
	*context = GINT_TO_POINTER(be_ndx);

	return LI_HANDLER_GO_ON;
}

static liHandlerResult balancer_act_fallback(liVRequest *vr, gboolean backlog_provided, gpointer param, gpointer *context, liBackendError error) {
	balancer *b = (balancer*) param;
	gint be_ndx = GPOINTER_TO_INT(*context);
	backend *be;
	gboolean debug = _OPTION(vr, b->p, 0).boolean;

	UNUSED(backlog_provided);

	if (be_ndx < 0) return LI_HANDLER_GO_ON;
	be = &g_array_index(b->backends, backend, be_ndx);

	if (debug || CORE_OPTION(LI_CORE_OPTION_DEBUG_REQUEST_HANDLING).boolean){
		VR_DEBUG(vr, "balancer fallback: %i (error: %i)", be_ndx, error);
	}

	g_mutex_lock(b->lock);

	be->load--;

	if (error == BACKEND_OVERLOAD || be->load > 0) {
		/* long timeout for overload - we will enable the backend anyway if another request finishs */
		if (be->state == BE_ALIVE) be->wake = ev_now(vr->wrk->loop) + 5.0;

		if (be->state != BE_DOWN) be->state = BE_OVERLOADED;
	} else {
		/* short timeout for dead backends - lets retry soon */
		be->wake = ev_now(vr->wrk->loop) + 1.0;

		be->state = BE_DOWN;
	}

	g_mutex_unlock(b->lock);

	*context = GINT_TO_POINTER(-1);

	return balancer_act_select(vr, backlog_provided, param, context);
}

static liHandlerResult balancer_act_finished(liVRequest *vr, gpointer param, gpointer context) {
	balancer *b = (balancer*) param;
	gint be_ndx = GPOINTER_TO_INT(context);
	backend *be;
	gboolean debug = _OPTION(vr, b->p, 0).boolean;

	UNUSED(vr);

	if (be_ndx < 0) return LI_HANDLER_GO_ON;
	be = &g_array_index(b->backends, backend, be_ndx);

	if (debug){
		VR_DEBUG(vr, "balancer finished: %i", be_ndx);
	}


	g_mutex_lock(b->lock);

	/* TODO implement backlog */
	be->load--;

	/* reactivate it (if not alive), as it obviously isn't completely down */
	be->state = BE_ALIVE;

	g_mutex_unlock(b->lock);

	return LI_HANDLER_GO_ON;
}

static void balancer_act_free(liServer *srv, gpointer param) {
	balancer_free(srv, (balancer*) param);
}

static liAction* balancer_create(liServer *srv, liWorker *wrk, liPlugin* p, liValue *val, gpointer userdata) {
	balancer *b;
	UNUSED(wrk); UNUSED(p); UNUSED(userdata);

	if (!val) {
		ERROR(srv, "%s", "need parameter");
		return NULL;
	}

	/* userdata contains the method */
	b = balancer_new(p, GPOINTER_TO_INT(userdata));
	if (!balancer_fill_backends(b, srv, val)) {
		balancer_free(srv, b);
		return NULL;
	}

	return li_action_new_balancer(balancer_act_select, balancer_act_fallback, balancer_act_finished, balancer_act_free, b, TRUE);
}

static const liPluginOption options[] = {
	{ "balancer.debug", LI_VALUE_BOOLEAN, FALSE, NULL },

	{ NULL, 0, 0, NULL }
};

static const liPluginAction actions[] = {
	{ "balancer.rr", balancer_create, GINT_TO_POINTER(BM_ROUNDROBIN) },
	{ "balancer.sqf", balancer_create, GINT_TO_POINTER(BM_SQF) },
	{ NULL, NULL, NULL }
};

static const liPluginSetup setups[] = {
	{ NULL, NULL, NULL }
};


static void plugin_init(liServer *srv, liPlugin *p, gpointer userdata) {
	UNUSED(srv); UNUSED(userdata);

	p->options = options;
	p->actions = actions;
	p->setups = setups;
}


gboolean mod_balancer_init(liModules *mods, liModule *mod) {
	MODULE_VERSION_CHECK(mods);

	mod->config = li_plugin_register(mods->main, "mod_balancer", plugin_init, NULL);

	return mod->config != NULL;
}

gboolean mod_balancer_free(liModules *mods, liModule *mod) {
	if (mod->config)
		li_plugin_free(mods->main, mod->config);

	return TRUE;
}

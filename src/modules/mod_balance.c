/*
 * mod_balance - balance between different backends
 *
 * Author:
 *     Copyright (c) 2009-2010 Stefan Bühler
 */


#include <lighttpd/base.h>
#include <lighttpd/plugin_core.h>

LI_API gboolean mod_balance_init(liModules *mods, liModule *mod);
LI_API gboolean mod_balance_free(liModules *mods, liModule *mod);

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

typedef struct backend backend;
typedef struct balancer balancer;
typedef struct bcontext bcontext;

struct backend {
	liAction *act;
	guint load;
	backend_state state;
	li_tstamp wake;
};

struct balancer {
	liWorker *wrk;

	GMutex *lock; /* balancer functions with "_" prefix need to be called with the lock being locked */
	GArray *backends;
	balancer_state state;
	balancer_method method;
	gint next_ndx;

	li_tstamp wake;

	liEventAsync async;
	gboolean delete_later; /* marked as "delete later in srv event loop" */

	GQueue backlog;
	gint backlog_limit;
	liEventTimer backlog_timer;
	gint backlog_reactivate_now;

	liPlugin *p;
};

struct bcontext { /* context for a balancer in a vrequest */
	gint selected; /* selected backend */

	GList backlog_link;
	liJobRef *ref;
	gboolean scheduled;
};

static void balancer_timer_cb(liEventBase *watcher, int events);
static void balancer_async_cb(liEventBase *watcher, int events);

static balancer* balancer_new(liWorker *wrk, liPlugin *p, balancer_method method) {
	balancer *b = g_slice_new0(balancer);
	b->wrk = wrk;
	b->lock = g_mutex_new();
	b->backends = g_array_new(FALSE, TRUE, sizeof(backend));
	b->method = method;
	b->state = BAL_ALIVE;
	b->p = p;

	b->backlog_limit = -1;

	li_event_timer_init(&wrk->loop, &b->backlog_timer, balancer_timer_cb);
	li_event_set_keep_loop_alive(&b->backlog_timer, FALSE);

	li_event_async_init(&wrk->loop, &b->async, balancer_async_cb);

	return b;
}

static void balancer_free(liServer *srv, balancer *b) {
	guint i;
	if (!b) return;
	g_mutex_free(b->lock);

	li_event_clear(&b->backlog_timer);
	li_event_clear(&b->async);

	for (i = 0; i < b->backends->len; i++) {
		backend *be = &g_array_index(b->backends, backend, i);
		li_action_release(srv, be->act);
	}
	g_array_free(b->backends, TRUE);
	g_slice_free(balancer, b);
}

static gboolean balancer_fill_backends(balancer *b, liServer *srv, liValue *val) {
	val = li_value_get_single_argument(val);

	if (LI_VALUE_ACTION == li_value_type(val)) {
		backend be;
		be.act = val->data.val_action.action;
		be.load = 0; be.state = BE_ALIVE; be.wake = 0;
		LI_FORCE_ASSERT(srv == val->data.val_action.srv);
		li_action_acquire(be.act);
		g_array_append_val(b->backends, be);
		return TRUE;
	} else if (LI_VALUE_LIST == li_value_type(val)) {
		if (li_value_list_has_len(val, 0)) {
			ERROR(srv, "%s", "expected non-empty list");
			return FALSE;
		}
		LI_VALUE_FOREACH(oa, val)
			if (LI_VALUE_ACTION != li_value_type(oa)) {
				ERROR(srv, "expected action at entry %u of list, got %s", _oa_i, li_value_type_string(oa));
				return FALSE;
			}
			LI_FORCE_ASSERT(srv == oa->data.val_action.srv);
			{
				backend be;
				be.act = oa->data.val_action.action;
				be.load = 0; be.state = BE_ALIVE; be.wake = 0;
				li_action_acquire(be.act);
				g_array_append_val(b->backends, be);
			}
		LI_VALUE_END_FOREACH()
		return TRUE;
	} else {
		ERROR(srv, "expected list, got %s", li_value_type_string(val));
		return FALSE;
	}
}

static void _balancer_context_backlog_unlink(balancer *b, bcontext *bc) {
	if (NULL != bc->backlog_link.data) {
		g_queue_unlink(&b->backlog, &bc->backlog_link);
		li_job_ref_release(bc->ref);
		bc->backlog_link.data = NULL;
		bc->backlog_link.next = bc->backlog_link.prev = NULL;
	}
}

static void _balancer_context_backlog_push(balancer *b, gpointer *context, liVRequest *vr) {
	bcontext *bc = *context;

	if (NULL == bc) {
		*context = bc = g_slice_new0(bcontext);
		bc->selected = -1;
	}

	if (NULL == bc->backlog_link.data) {
		bc->ref = li_vrequest_get_ref(vr);
		bc->backlog_link.data = bc;
		if (bc->scheduled) {
			/* higher priority: has already been waiting and was scheduled */
			g_queue_push_head_link(&b->backlog, &bc->backlog_link);
		} else {
			g_queue_push_tail_link(&b->backlog, &bc->backlog_link);
		}
		bc->scheduled = 0; /* reset scheduled flag */
	}
}

/* returns FALSE if b was destroyed (only possible in event loop) */
static gboolean _balancer_backlog_update_watcher(liWorker *wrk, balancer *b) {
	if (wrk != b->wrk) {
		li_event_async_send(&b->async);
		return TRUE;
	}

	if (b->delete_later) {
		g_mutex_unlock(b->lock);
		balancer_free(wrk->srv, b);
		return FALSE;
	}

	if (b->state == BAL_ALIVE) {
		li_event_stop(&b->backlog_timer);
	} else {
		li_event_timer_once(&b->backlog_timer, b->wake - li_cur_ts(wrk));
	}

	return TRUE;
}

/* returns FALSE if b was destroyed (only possible in event loop)  */
static gboolean _balancer_backlog_schedule(liWorker *wrk, balancer *b) {
	GList *it;

	while (b->backlog_reactivate_now > 0) {
		bcontext *bc;
		it = g_queue_peek_head_link(&b->backlog);

		if (NULL == it) {
			/* backlog done */
			b->state = BAL_ALIVE;
			b->backlog_reactivate_now = 0;
			b->wake = 0;

			return _balancer_backlog_update_watcher(wrk, b);
		}

		bc = it->data;
		bc->scheduled = 1;

		li_job_async(bc->ref);

		g_queue_unlink(&b->backlog, it);
		li_job_ref_release(bc->ref);
		it->data = NULL;
		it->next = it->prev = NULL;
	}

	return _balancer_backlog_update_watcher(wrk, b);
}

static void balancer_timer_cb(liEventBase *watcher, int events) {
	balancer *b = LI_CONTAINER_OF(li_event_timer_from(watcher), balancer, backlog_timer);
	int n;
	UNUSED(events);

	g_mutex_lock(b->lock);

	n = b->backends->len / 2;
	if (n == 0) n = 1;
	b->backlog_reactivate_now += n;

	if (!_balancer_backlog_schedule(b->wrk, b)) return;

	g_mutex_unlock(b->lock);
}

static void balancer_async_cb(liEventBase *watcher, int events) {
	balancer *b = LI_CONTAINER_OF(li_event_async_from(watcher), balancer, async);
	UNUSED(events);

	g_mutex_lock(b->lock);

	if (!_balancer_backlog_update_watcher(b->wrk, b)) return;

	g_mutex_unlock(b->lock);
}

static void balancer_context_free(liVRequest *vr, balancer *b, gpointer *context, gboolean success) {
	bcontext *bc = *context;

	if (!bc) return;
	*context = NULL;

	g_mutex_lock(b->lock);

	_balancer_context_backlog_unlink(b, bc);

	if (bc->selected >= 0) {
		backend *be = &g_array_index(b->backends, backend, bc->selected);
		be->load--;
		bc->selected = -1;

		if (success) {
			/* reactivate it (if not alive), as it obviously isn't completely down */
			be->state = BE_ALIVE;
			b->backlog_reactivate_now++;
			_balancer_backlog_schedule(vr->wrk, b);
		}
	}

	g_mutex_unlock(b->lock);

	g_slice_free(bcontext, bc);
}


static void _balancer_context_select_backend(balancer *b, gpointer *context, gint ndx) {
	bcontext *bc = *context;

	if (NULL == bc) {
		*context = bc = g_slice_new0(bcontext);
		bc->selected = -1;
	}

	_balancer_context_backlog_unlink(b, bc);

	if (bc->selected >= 0) {
		backend *be = &g_array_index(b->backends, backend, bc->selected);
		be->load--;
	}

	bc->selected = ndx;

	if (bc->selected >= 0) {
		backend *be = &g_array_index(b->backends, backend, bc->selected);
		be->load++;
		b->next_ndx = ndx + 1;
	}
}

static liHandlerResult balancer_act_select(liVRequest *vr, gboolean backlog_provided, gpointer param, gpointer *context) {
	balancer *b = param;
	bcontext *bc = *context;
	gint be_ndx, load;
	guint i, j;
	backend *be;
	li_tstamp now = li_cur_ts(vr->wrk);
	gboolean all_dead = TRUE;
	gboolean debug = _OPTION(vr, b->p, 0).boolean;

	be_ndx = -1;

	g_mutex_lock(b->lock);

	if (b->state != BAL_ALIVE && backlog_provided) {
		/* don't use own backlog if someone else before us does provide it */
		if (b->state == BAL_DOWN) {
			li_vrequest_backend_dead(vr);
		} else {
			li_vrequest_backend_overloaded(vr);
		}
	}

	/* don't backlog scheduled requests */
	if ((NULL == bc || !bc->scheduled) && b->backlog.length > 0) {
		if (-1 == b->backlog_limit || (gint)b->backlog.length < b->backlog_limit) {
			/* backlog not full yet */
			_balancer_context_backlog_push(b, context, vr);

			g_mutex_unlock(b->lock);

			return LI_HANDLER_WAIT_FOR_EVENT;
		}

		/* backlog full / no backlog */
		if (b->state == BAL_DOWN) {
			li_vrequest_backend_dead(vr);
		} else {
			li_vrequest_backend_overloaded(vr);
		}

		g_mutex_unlock(b->lock);

		return LI_HANDLER_GO_ON;
	}

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
		/* Couldn't find active backend */

		if (b->state == BAL_ALIVE) {
			b->state = all_dead ? BAL_DOWN : BAL_OVERLOADED;
			b->wake = li_cur_ts(vr->wrk) + 10;

			for (i = 0; i < b->backends->len; i++) {
				be = &g_array_index(b->backends, backend, i);

				if (b->wake > be->wake) b->wake = be->wake;
			}

			/* start backlog now */
			b->backlog_reactivate_now = 0;
			_balancer_backlog_update_watcher(vr->wrk, b);
		}

		if (-1 == b->backlog_limit || (gint)b->backlog.length < b->backlog_limit) {
			_balancer_context_backlog_push(b, context, vr);

			g_mutex_unlock(b->lock);

			return LI_HANDLER_WAIT_FOR_EVENT;
		}

		if (all_dead) {
			li_vrequest_backend_dead(vr);
		} else {
			li_vrequest_backend_overloaded(vr);
		}

		g_mutex_unlock(b->lock);

		return LI_HANDLER_GO_ON;
	}

	_balancer_context_select_backend(b, context, be_ndx);
	be = &g_array_index(b->backends, backend, be_ndx);

	g_mutex_unlock(b->lock);

	if (debug || CORE_OPTION(LI_CORE_OPTION_DEBUG_REQUEST_HANDLING).boolean){
		VR_DEBUG(vr, "balancer select: %i", be_ndx);
	}

	li_action_enter(vr, be->act);

	return LI_HANDLER_GO_ON;
}

static liHandlerResult balancer_act_fallback(liVRequest *vr, gboolean backlog_provided, gpointer param, gpointer *context, liBackendError error) {
	balancer *b = param;
	bcontext *bc = *context;
	backend *be;
	gboolean debug = _OPTION(vr, b->p, 0).boolean;

	if (!bc || bc->selected < 0) return LI_HANDLER_GO_ON;
	be = &g_array_index(b->backends, backend, bc->selected);

	if (debug || CORE_OPTION(LI_CORE_OPTION_DEBUG_REQUEST_HANDLING).boolean){
		VR_DEBUG(vr, "balancer fallback: %i (error: %i)", bc->selected, error);
	}

	g_mutex_lock(b->lock);

	_balancer_context_select_backend(b, context, -1);

	if (error == LI_BACKEND_OVERLOAD || be->load > 0) {
		/* long timeout for overload - we will enable the backend anyway if another request finishs */
		if (be->state == BE_ALIVE) be->wake = li_cur_ts(vr->wrk) + 5.0;

		if (be->state != BE_DOWN) be->state = BE_OVERLOADED;
	} else {
		/* short timeout for dead backends - lets retry soon */
		be->wake = li_cur_ts(vr->wrk) + 1.0;

		be->state = BE_DOWN;
	}


	if (b->wake > be->wake) b->wake = be->wake;

	g_mutex_unlock(b->lock);

	return balancer_act_select(vr, backlog_provided, param, context);
}

static liHandlerResult balancer_act_finished(liVRequest *vr, gpointer param, gpointer context) {
	balancer *b = param;
	bcontext *bc = context;
	gboolean debug = _OPTION(vr, b->p, 0).boolean;

	if (!bc) return LI_HANDLER_GO_ON;

	if (debug && bc->selected >= 0) {
		VR_DEBUG(vr, "balancer finished: %i", bc->selected);
	}

	balancer_context_free(vr, b, &context, TRUE);

	return LI_HANDLER_GO_ON;
}

static void balancer_act_free(liServer *srv, gpointer param) {
	balancer *b = param;
	UNUSED(srv);

	g_mutex_lock(b->lock);

	b->delete_later = TRUE;
	_balancer_backlog_update_watcher(NULL, b);

	g_mutex_unlock(b->lock);
}

static liAction* balancer_create(liServer *srv, liWorker *wrk, liPlugin* p, liValue *val, gpointer userdata) {
	balancer *b;

	if (NULL == val) {
		ERROR(srv, "%s", "need parameter");
		return NULL;
	}

	/* userdata contains the method */
	b = balancer_new(wrk, p, GPOINTER_TO_INT(userdata));
	if (!balancer_fill_backends(b, srv, val)) {
		balancer_free(srv, b);
		return NULL;
	}

	return li_action_new_balancer(balancer_act_select, balancer_act_fallback, balancer_act_finished, balancer_act_free, b, TRUE);
}

static const liPluginOption options[] = {
	{ "balance.debug", LI_VALUE_BOOLEAN, FALSE, NULL },

	{ NULL, 0, 0, NULL }
};

static const liPluginAction actions[] = {
	{ "balance.rr", balancer_create, GINT_TO_POINTER(BM_ROUNDROBIN) },
	{ "balance.sqf", balancer_create, GINT_TO_POINTER(BM_SQF) },
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


gboolean mod_balance_init(liModules *mods, liModule *mod) {
	MODULE_VERSION_CHECK(mods);

	mod->config = li_plugin_register(mods->main, "mod_balance", plugin_init, NULL);

	return mod->config != NULL;
}

gboolean mod_balance_free(liModules *mods, liModule *mod) {
	if (mod->config)
		li_plugin_free(mods->main, mod->config);

	return TRUE;
}


#include <lighttpd/base.h>

typedef struct action_stack_element action_stack_element;

struct action_stack_element {
	liAction *act;
	union {
		gpointer context;
		guint pos;
	} data;
	gboolean finished, backlog_provided;
};

void li_action_release(liServer *srv, liAction *a) {
	guint i;
	if (!a) return;
	assert(g_atomic_int_get(&a->refcount) > 0);
	if (g_atomic_int_dec_and_test(&a->refcount)) {
		switch (a->type) {
		case ACTION_TSETTING:
			break;
		case ACTION_TSETTINGPTR:
			li_release_optionptr(srv, a->data.settingptr.value);
			break;
		case ACTION_TFUNCTION:
			if (a->data.function.free) {
				a->data.function.free(srv, a->data.function.param);
			}
			break;
		case ACTION_TCONDITION:
			li_condition_release(srv, a->data.condition.cond);
			li_action_release(srv, a->data.condition.target);
			li_action_release(srv, a->data.condition.target_else);
			break;
		case ACTION_TLIST:
			for (i = a->data.list->len; i-- > 0; ) {
				li_action_release(srv, g_array_index(a->data.list, liAction*, i));
			}
			g_array_free(a->data.list, TRUE);
			break;
		case ACTION_TBALANCER:
			if (a->data.balancer.free) {
				a->data.balancer.free(srv, a->data.balancer.param);
			}
			break;
		}
		g_slice_free(liAction, a);
	}
}

void li_action_acquire(liAction *a) {
	assert(g_atomic_int_get(&a->refcount) > 0);
	g_atomic_int_inc(&a->refcount);
}

liAction *li_action_new_setting(liOptionSet setting) {
	liAction *a = g_slice_new(liAction);

	a->refcount = 1;
	a->type = ACTION_TSETTING;
	a->data.setting = setting;

	return a;
}

liAction *li_action_new_settingptr(liOptionPtrSet setting) {
	liAction *a = g_slice_new(liAction);

	a->refcount = 1;
	a->type = ACTION_TSETTINGPTR;
	a->data.settingptr = setting;

	return a;
}

liAction *li_action_new_function(liActionFuncCB func, liActionCleanupCB fcleanup, liActionFreeCB ffree, gpointer param) {
	liAction *a;

	a = g_slice_new(liAction);
	a->refcount = 1;
	a->type = ACTION_TFUNCTION;
	a->data.function.func = func;
	a->data.function.cleanup = fcleanup;
	a->data.function.free = ffree;
	a->data.function.param = param;

	return a;
}

liAction *li_action_new_list() {
	liAction *a;

	a = g_slice_new(liAction);
	a->refcount = 1;
	a->type = ACTION_TLIST;
	a->data.list = g_array_new(FALSE, TRUE, sizeof(liAction *));

	return a;
}

liAction *li_action_new_condition(liCondition *cond, liAction *target, liAction *target_else) {
	liAction *a;

	a = g_slice_new(liAction);
	a->refcount = 1;
	a->type = ACTION_TCONDITION;
	a->data.condition.cond = cond;
	a->data.condition.target = target;
	a->data.condition.target_else = target_else;

	return a;
}

liAction *li_action_new_balancer(liBackendSelectCB bselect, liBackendFallbackCB bfallback, liBackendFinishedCB bfinished, liBalancerFreeCB bfree, gpointer param, gboolean provide_backlog) {
	liAction *a;

	a = g_slice_new(liAction);
	a->refcount = 1;
	a->type = ACTION_TBALANCER;
	a->data.balancer.select = bselect;
	a->data.balancer.fallback = bfallback;
	a->data.balancer.finished = bfinished;
	a->data.balancer.free = bfree;
	a->data.balancer.param = param;
	a->data.balancer.provide_backlog = provide_backlog;

	return a;
}

static void action_stack_element_release(liServer *srv, liVRequest *vr, action_stack_element *ase) {
	liAction *a = ase->act;

	if (!ase || !a) return;

	switch (a->type) {
	case ACTION_TSETTING:
	case ACTION_TSETTINGPTR:
		break;
	case ACTION_TFUNCTION:
		if (ase->data.context && a->data.function.cleanup) {
			a->data.function.cleanup(vr, a->data.function.param, ase->data.context);
		}
		break;
	case ACTION_TCONDITION:
		if (a->data.condition.cond->rvalue.type == LI_COND_VALUE_REGEXP) {
			/* pop regex stack */
			GArray *rs = vr->action_stack.regex_stack;
			/* cheap check to prevent segfault if condition errored without pushing onto stack; whole stack gets cleaned anyways */
			if (rs->len) {
				liActionRegexStackElement *arse = &g_array_index(rs, liActionRegexStackElement, rs->len - 1);
				if (arse->string)
					g_string_free(arse->string, TRUE);
				g_match_info_free(arse->match_info);
				g_array_set_size(rs, rs->len - 1);
			}
		}
		break;
	case ACTION_TLIST:
		break;
	case ACTION_TBALANCER:
		a->data.balancer.finished(vr, a->data.balancer.param, ase->data.context);
		break;
	}

	li_action_release(srv, ase->act);
	ase->act = NULL;
	ase->data.context = NULL;
}

void li_action_stack_init(liActionStack *as) {
	as->stack = g_array_sized_new(FALSE, TRUE, sizeof(action_stack_element), 16);
	as->regex_stack = g_array_sized_new(FALSE, FALSE, sizeof(liActionRegexStackElement), 16);
	as->backend_stack = g_array_sized_new(FALSE, TRUE, sizeof(action_stack_element), 4);
}

static void li_action_backend_stack_reset(liVRequest *vr, liActionStack *as) {
	liServer *srv = vr->wrk->srv;
	guint i;

	/* index 0 is the "deepest" backend - release it first */
	for (i = 0; i < as->backend_stack->len; i++ ) {
		action_stack_element_release(srv, vr, &g_array_index(as->backend_stack, action_stack_element, i));
	}
	g_array_set_size(as->backend_stack, 0);
}

void li_action_stack_reset(liVRequest *vr, liActionStack *as) {
	liServer *srv = vr->wrk->srv;
	guint i;

	for (i = as->stack->len; i-- > 0; ) {
		action_stack_element_release(srv, vr, &g_array_index(as->stack, action_stack_element, i));
	}
	g_array_set_size(as->stack, 0);

	li_action_backend_stack_reset(vr, as);

	as->backend_failed = FALSE;
	as->backend_finished = FALSE;
}

void li_action_stack_clear(liVRequest *vr, liActionStack *as) {
	liServer *srv = vr->wrk->srv;
	guint i;

	for (i = as->stack->len; i-- > 0; ) {
		action_stack_element_release(srv, vr, &g_array_index(as->stack, action_stack_element, i));
	}
	g_array_free(as->stack, TRUE);

	li_action_backend_stack_reset(vr, as);
	g_array_free(as->backend_stack, TRUE);

	g_array_free(as->regex_stack, TRUE);

	as->stack = as->backend_stack = as->regex_stack = NULL;
	as->backend_failed = FALSE;
	as->backend_finished = FALSE;
}

static action_stack_element *action_stack_top(liActionStack* as) {
	return as->stack->len > 0 ? &g_array_index(as->stack, action_stack_element, as->stack->len - 1) : NULL;
}

/** handle sublist now, remember current position (stack) */
void li_action_enter(liVRequest *vr, liAction *a) {
	liActionStack *as = &vr->action_stack;
	action_stack_element *top_ase = action_stack_top(as);
	action_stack_element ase = { a, { 0 }, FALSE,
		(top_ase ? top_ase->backlog_provided || (top_ase->act->type == ACTION_TBALANCER && top_ase->act->data.balancer.provide_backlog) : FALSE) };
	li_action_acquire(a);
	g_array_append_val(as->stack, ase);
}

static void action_stack_pop(liServer *srv, liVRequest *vr, liActionStack *as) {
	action_stack_element *ase;

	if (as->stack->len == 0) return;

	ase = &g_array_index(as->stack, action_stack_element, as->stack->len - 1);

	if (ase->act->type == ACTION_TBALANCER && !as->backend_finished) {
		/* release later if backend is finished (i.e. "disconnected") */
		g_array_append_val(as->backend_stack, *ase);
	} else {
		action_stack_element_release(srv, vr, ase);
	}

	g_array_set_size(as->stack, as->stack->len - 1);
}

liHandlerResult li_action_execute(liVRequest *vr) {
	liAction *a;
	liActionStack *as = &vr->action_stack;
	action_stack_element *ase;
	guint ase_ndx;
	liHandlerResult res;
	gboolean condres;
	liServer *srv = vr->wrk->srv;

	while (NULL != (ase = action_stack_top(as))) {
		if (as->backend_failed) {
			/* set by li_vrequest_backend_error */
			vr->state = LI_VRS_HANDLE_REQUEST_HEADERS;
			vr->backend = NULL;

			/* pop top action in every case (if the balancer itself failed we don't want to restart it) */
			action_stack_pop(srv, vr, as);
			while (NULL != (ase = action_stack_top(as)) && (ase->act->type != ACTION_TBALANCER || !ase->act->data.balancer.provide_backlog)) {
				action_stack_pop(srv, vr, as);
			}
			if (!ase) { /* no backlogging balancer found */
				if (li_vrequest_handle_direct(vr))
					vr->response.http_status = 503;
				return LI_HANDLER_GO_ON;
			}
			as->backend_failed = FALSE;
			
			ase->finished = FALSE;
			a = ase->act;
			res = a->data.balancer.fallback(vr, ase->backlog_provided, a->data.balancer.param, &ase->data.context, as->backend_error);
			switch (res) {
			case LI_HANDLER_GO_ON:
				ase->finished = TRUE;
				break;
			case LI_HANDLER_ERROR:
				li_action_stack_reset(vr, as);
			case LI_HANDLER_COMEBACK:
			case LI_HANDLER_WAIT_FOR_EVENT:
				return res;
			}
			continue;
		}
		if (ase->finished) {
			/* a TFUNCTION may enter sub actions _and_ return GO_ON, so we cannot pop the last element
			 * but we have to remember we already executed it
			 */
			if (ase->act->type == ACTION_TBALANCER) {
				/* wait until we found a backend */
				VREQUEST_WAIT_FOR_RESPONSE_HEADERS(vr);
			}
			action_stack_pop(srv, vr, as);
			continue;
		}

		vr->wrk->stats.actions_executed++;
		a = ase->act;
		ase_ndx = as->stack->len - 1; /* sometimes the stack gets modified - reread "ase" after that */

		switch (a->type) {
		case ACTION_TSETTING:
			vr->options[a->data.setting.ndx] = a->data.setting.value;
			action_stack_pop(srv, vr, as);
			break;
		case ACTION_TSETTINGPTR:
			if (vr->optionptrs[a->data.settingptr.ndx] != a->data.settingptr.value) {
				g_atomic_int_inc(&a->data.settingptr.value->refcount);
				li_release_optionptr(srv, vr->optionptrs[a->data.settingptr.ndx]);
				vr->optionptrs[a->data.settingptr.ndx] = a->data.settingptr.value;
			}
			action_stack_pop(srv, vr, as);
			break;
		case ACTION_TFUNCTION:
			res = a->data.function.func(vr, a->data.function.param, &ase->data.context);
			ase = &g_array_index(as->stack, action_stack_element, ase_ndx);

			switch (res) {
			case LI_HANDLER_GO_ON:
				ase->finished = TRUE;
				break;
			case LI_HANDLER_ERROR:
				li_action_stack_reset(vr, as);
			case LI_HANDLER_COMEBACK:
			case LI_HANDLER_WAIT_FOR_EVENT:
				if (ase != action_stack_top(as)) break; /* allow an action to push another action and rerun after it again */
				return res;
			}
			break;
		case ACTION_TCONDITION:
			condres = FALSE;
			res = li_condition_check(vr, a->data.condition.cond, &condres);
			switch (res) {
			case LI_HANDLER_GO_ON:
				ase->finished = TRUE;
				if (condres) {
					if (a->data.condition.target) li_action_enter(vr, a->data.condition.target);
				}
				else if (a->data.condition.target_else) {
					li_action_enter(vr, a->data.condition.target_else);
				}
				break;
			case LI_HANDLER_ERROR:
				li_action_stack_reset(vr, as);
			case LI_HANDLER_COMEBACK:
			case LI_HANDLER_WAIT_FOR_EVENT:
				return res;
			}
			break;
		case ACTION_TLIST:
			if (ase->data.pos >= a->data.list->len) {
				action_stack_pop(srv, vr, as);
			} else {
				guint p = ase->data.pos++;
				li_action_enter(vr, g_array_index(a->data.list, liAction*, p));
			}
			break;
		case ACTION_TBALANCER:
			/* skip balancer if request is already handled */
			if (li_vrequest_is_handled(vr)) {
				ase->finished = TRUE;
				break;
			}
			res = a->data.balancer.select(vr, ase->backlog_provided, a->data.balancer.param, &ase->data.context);
			ase = &g_array_index(as->stack, action_stack_element, ase_ndx);
			switch (res) {
			case LI_HANDLER_GO_ON:
				ase->finished = TRUE;
				break;
			case LI_HANDLER_ERROR:
				li_action_stack_reset(vr, as);
			case LI_HANDLER_COMEBACK:
			case LI_HANDLER_WAIT_FOR_EVENT:
				return res;
			}
			break;
		}
	}
	if (as->backend_failed) {
		if (li_vrequest_handle_direct(vr))
			vr->response.http_status = 503;
	}
	return LI_HANDLER_GO_ON;
}

void li_vrequest_backend_finished(liVRequest *vr) {
	if (!li_vrequest_is_handled(vr)) return;
	vr->action_stack.backend_finished = TRUE;
	li_action_backend_stack_reset(vr, &vr->action_stack);
}

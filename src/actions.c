
#include <lighttpd/base.h>

struct action_stack_element;
typedef struct action_stack_element action_stack_element;

struct action_stack_element {
	action *act;
	union {
		gpointer context;
		guint pos;
	} data;
	gboolean finished;
};

void action_release(server *srv, action *a) {
	if (!a) return;
	guint i;
	assert(g_atomic_int_get(&a->refcount) > 0);
	if (g_atomic_int_dec_and_test(&a->refcount)) {
		switch (a->type) {
		case ACTION_TSETTING:
			release_option(srv, &a->data.setting);
			break;
		case ACTION_TFUNCTION:
			if (a->data.function.free) {
				a->data.function.free(srv, a->data.function.param);
			}
			break;
		case ACTION_TCONDITION:
			condition_release(srv, a->data.condition.cond);
			action_release(srv, a->data.condition.target);
			action_release(srv, a->data.condition.target_else);
			break;
		case ACTION_TLIST:
			for (i = a->data.list->len; i-- > 0; ) {
				action_release(srv, g_array_index(a->data.list, action*, i));
			}
			g_array_free(a->data.list, TRUE);
			break;
		}
		g_slice_free(action, a);
	}
}

void action_acquire(action *a) {
	assert(g_atomic_int_get(&a->refcount) > 0);
	g_atomic_int_inc(&a->refcount);
}

action *action_new_setting(option_set setting) {
	action *a = g_slice_new(action);

	a->refcount = 1;
	a->type = ACTION_TSETTING;
	a->data.setting = setting;

	return a;
}

action *action_new_function(ActionFunc func, ActionCleanup fcleanup, ActionFree ffree, gpointer param) {
	action *a;

	a = g_slice_new(action);
	a->refcount = 1;
	a->type = ACTION_TFUNCTION;
	a->data.function.func = func;
	a->data.function.cleanup = fcleanup;
	a->data.function.free = ffree;
	a->data.function.param = param;

	return a;
}

action *action_new_list() {
	action *a;

	a = g_slice_new(action);
	a->refcount = 1;
	a->type = ACTION_TLIST;
	a->data.list = g_array_new(FALSE, TRUE, sizeof(action *));

	return a;
}

action *action_new_condition(condition *cond, action *target, action *target_else) {
	action *a;

	a = g_slice_new(action);
	a->refcount = 1;
	a->type = ACTION_TCONDITION;
	a->data.condition.cond = cond;
	a->data.condition.target = target;
	a->data.condition.target_else = target_else;

	return a;
}

static void action_stack_element_release(server *srv, vrequest *vr, action_stack_element *ase) {
	action *a = ase->act;
	if (!ase || !a) return;
	switch (a->type) {
	case ACTION_TSETTING:
		break;
	case ACTION_TFUNCTION:
		if (ase->data.context && a->data.function.cleanup) {
			a->data.function.cleanup(vr, a->data.function.param, ase->data.context);
		}
		break;
	case ACTION_TCONDITION:
	case ACTION_TLIST:
		break;
	case ACTION_TBALANCER:
		a->data.balancer.finished(vr, a->data.balancer.param, ase->data.context);
		break;
	}
	action_release(srv, ase->act);
	ase->act = NULL;
	ase->data.context = NULL;
}

void action_stack_init(action_stack *as) {
	as->stack = g_array_sized_new(FALSE, TRUE, sizeof(action_stack_element), 15);
}

void action_stack_reset(vrequest *vr, action_stack *as) {
	server *srv = vr->con->srv;
	guint i;
	for (i = as->stack->len; i-- > 0; ) {
		action_stack_element_release(srv, vr, &g_array_index(as->stack, action_stack_element, i));
	}
	g_array_set_size(as->stack, 0);
}

void action_stack_clear(vrequest *vr, action_stack *as) {
	server *srv = vr->con->srv;
	guint i;
	for (i = as->stack->len; i-- > 0; ) {
		action_stack_element_release(srv, vr, &g_array_index(as->stack, action_stack_element, i));
	}
	g_array_free(as->stack, TRUE);
	as->stack = NULL;
}

/** handle sublist now, remember current position (stack) */
void action_enter(vrequest *vr, action *a) {
	action_acquire(a);
	action_stack_element ase = { a, { 0 }, FALSE };
	g_array_append_val(vr->action_stack.stack, ase);
}

static action_stack_element *action_stack_top(action_stack* as) {
	return as->stack->len > 0 ? &g_array_index(as->stack, action_stack_element, as->stack->len - 1) : NULL;
}

static void action_stack_pop(server *srv, vrequest *vr, action_stack *as) {
	action_stack_element_release(srv, vr, &g_array_index(as->stack, action_stack_element, as->stack->len - 1));
	g_array_set_size(as->stack, as->stack->len - 1);
}

handler_t action_execute(vrequest *vr) {
	action *a;
	action_stack *as = &vr->action_stack;
	action_stack_element *ase;
	handler_t res;
	gboolean condres;
	server *srv = vr->con->srv;

	while (NULL != (ase = action_stack_top(as))) {
		if (ase->finished) {
			/* a TFUNCTION may enter sub actions _and_ return GO_ON, so we cannot pop the last element
			 * but we have to remember we already executed it
			 */
			action_stack_pop(srv, vr, as);
			continue;
		}

		a = ase->act;
		vr->con->wrk->stats.actions_executed++;

		switch (a->type) {
		case ACTION_TSETTING:
			vr->options[a->data.setting.ndx] = a->data.setting.value;
			action_stack_pop(srv, vr, as);
			break;
		case ACTION_TFUNCTION:
			res = a->data.function.func(vr, a->data.function.param, &ase->data.context);
			switch (res) {
			case HANDLER_GO_ON:
				ase->finished = TRUE;
				break;
			case HANDLER_ERROR:
				action_stack_reset(vr, as);
			case HANDLER_COMEBACK:
			case HANDLER_WAIT_FOR_EVENT:
			case HANDLER_WAIT_FOR_FD:
				return res;
			}
			break;
		case ACTION_TCONDITION:
			condres = FALSE;
			res = condition_check(vr, a->data.condition.cond, &condres);
			switch (res) {
			case HANDLER_GO_ON:
				action_stack_pop(srv, vr, as);
				if (condres) {
					action_enter(vr, a->data.condition.target);
				}
				else if (a->data.condition.target_else) {
					action_enter(vr, a->data.condition.target_else);
				}
				break;
			case HANDLER_ERROR:
				action_stack_reset(vr, as);
			case HANDLER_COMEBACK:
			case HANDLER_WAIT_FOR_EVENT:
			case HANDLER_WAIT_FOR_FD:
				return res;
			}
			break;
		case ACTION_TLIST:
			if (ase->data.pos >= a->data.list->len) {
				action_stack_pop(srv, vr, as);
			} else {
				action_enter(vr, g_array_index(a->data.list, action*, ase->data.pos));
				ase->data.pos++;
			}
			break;
		case ACTION_TBALANCER:
			res = a->data.balancer.select(vr, a->data.balancer.param, &ase->data.context);
			switch (res) {
			case HANDLER_GO_ON:
				ase->finished = TRUE;
				break;
			case HANDLER_ERROR:
				action_stack_reset(vr, as);
			case HANDLER_COMEBACK:
			case HANDLER_WAIT_FOR_EVENT:
			case HANDLER_WAIT_FOR_FD:
				return res;
			}
			break;
		}
	}
	return HANDLER_GO_ON;
}

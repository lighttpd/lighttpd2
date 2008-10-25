
#include "log.h"
#include "actions.h"
#include "condition.h"

struct action_stack_element;
typedef struct action_stack_element action_stack_element;

struct action_stack_element {
	action *act;
	guint pos;
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

action *action_new_function(ActionFunc func, ActionFree ffree, gpointer param) {
	action *a;

	a = g_slice_new(action);
	a->refcount = 1;
	a->type = ACTION_TFUNCTION;
	a->data.function.func = func;
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

void action_stack_element_release(server *srv, action_stack_element *ase) {
	if (!ase || !ase->act) return;
	action_release(srv, ase->act);
	ase->act = NULL;
}

void action_stack_init(action_stack *as) {
	as->stack = g_array_sized_new(FALSE, TRUE, sizeof(action_stack_element), 15);
}

void action_stack_reset(server *srv, action_stack *as) {
	guint i;
	for (i = as->stack->len; i-- > 0; ) {
		action_stack_element_release(srv, &g_array_index(as->stack, action_stack_element, i));
	}
	g_array_set_size(as->stack, 0);
}

void action_stack_clear(server *srv, action_stack *as) {
	guint i;
	for (i = as->stack->len; i-- > 0; ) {
		action_stack_element_release(srv, &g_array_index(as->stack, action_stack_element, i));
	}
	g_array_free(as->stack, TRUE);
	as->stack = NULL;
}

/** handle sublist now, remember current position (stack) */
void action_enter(vrequest *vr, action *a) {
	action_acquire(a);
	action_stack_element ase = { a, 0 };
	g_array_append_val(vr->action_stack.stack, ase);
}

static action_stack_element *action_stack_top(action_stack* as) {
	return as->stack->len > 0 ? &g_array_index(as->stack, action_stack_element, as->stack->len - 1) : NULL;
}

static void action_stack_pop(server *srv, action_stack *as) {
	action_stack_element_release(srv, &g_array_index(as->stack, action_stack_element, as->stack->len - 1));
	g_array_set_size(as->stack, as->stack->len - 1);
}

static action* action_stack_element_action(action_stack_element *ase) {
	action *a = ase->act;
	if (a->type == ACTION_TLIST) {
		return ase->pos < a->data.list->len ? g_array_index(a->data.list, action*, ase->pos) : NULL;
	} else {
		return ase->pos == 0 ? a : NULL;
	}
}

handler_t action_execute(vrequest *vr) {
	action *a;
	action_stack *as = &vr->action_stack;
	action_stack_element *ase;
	handler_t res;
	gboolean condres;

	while (NULL != (ase = action_stack_top(as))) {
		a = action_stack_element_action(ase);
		if (!a) {
			action_stack_pop(vr->con->srv, as);
			continue;
		}

		vr->con->wrk->stats.actions_executed++;

		switch (a->type) {
		case ACTION_TSETTING:
			vr->con->options[a->data.setting.ndx] = a->data.setting.value;
			break;
		case ACTION_TFUNCTION:
			res = a->data.function.func(vr, a->data.function.param);
			switch (res) {
			case HANDLER_GO_ON:
			case HANDLER_FINISHED:
				break;
			case HANDLER_ERROR:
				action_stack_reset(vr->con->srv, as);
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
			case HANDLER_FINISHED:
				if (condres) {
					action_enter(vr, a->data.condition.target);
				}
				else if (a->data.condition.target_else) {
					action_enter(vr, a->data.condition.target_else);
				}
				break;
			case HANDLER_ERROR:
				action_stack_reset(vr->con->srv, as);
			case HANDLER_COMEBACK:
			case HANDLER_WAIT_FOR_EVENT:
			case HANDLER_WAIT_FOR_FD:
				return res;
			}
			break;
		case ACTION_TLIST:
			action_enter(vr, a);
			break;
		}
		ase->pos++;
	}
	return HANDLER_FINISHED;
}

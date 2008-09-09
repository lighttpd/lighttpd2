
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
			release_option(srv, &a->value.setting);
			break;
		case ACTION_TFUNCTION:
			if (a->value.function.free) {
				a->value.function.free(srv, a->value.function.param);
			}
			break;
		case ACTION_TCONDITION:
			condition_release(srv, a->value.condition.cond);
			action_release(srv, a->value.condition.target);
			action_release(srv, a->value.condition.target_else);
			break;
		case ACTION_TLIST:
			for (i = a->value.list->len; i-- > 0; ) {
				action_release(srv, g_array_index(a->value.list, action*, i));
			}
			g_array_free(a->value.list, TRUE);
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
	a->value.setting = setting;

	return a;
}

action *action_new_function(ActionFunc func, ActionFree ffree, gpointer param) {
	action *a;

	a = g_slice_new(action);
	a->refcount = 1;
	a->type = ACTION_TFUNCTION;
	a->value.function.func = func;
	a->value.function.free = ffree;
	a->value.function.param = param;

	return a;
}

action *action_new_list() {
	action *a;

	a = g_slice_new(action);
	a->refcount = 1;
	a->type = ACTION_TLIST;
	a->value.list = g_array_new(FALSE, TRUE, sizeof(action *));

	return a;
}

action *action_new_condition(condition *cond, action *target, action *target_else) {
	action *a;

	a = g_slice_new(action);
	a->refcount = 1;
	a->type = ACTION_TCONDITION;
	a->value.condition.cond = cond;
	a->value.condition.target = target;
	a->value.condition.target_else = target_else;

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
void action_enter(connection *con, action *a) {
	action_acquire(a);
	action_stack_element ase = { a, 0 };
	g_array_append_val(con->action_stack.stack, ase);
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
		return ase->pos < a->value.list->len ? g_array_index(a->value.list, action*, ase->pos) : NULL;
	} else {
		return ase->pos == 0 ? a : NULL;
	}
}

action_result action_execute(connection *con) {
	action *a;
	action_stack *as = &con->action_stack;
	action_stack_element *ase;
	action_result res;

	while (NULL != (ase = action_stack_top(as))) {
		a = action_stack_element_action(ase);
		if (!a) {
			action_stack_pop(con->srv, as);
			continue;
		}

		con->srv->stats.actions_executed++;

		switch (a->type) {
		case ACTION_TSETTING:
			con->options[a->value.setting.ndx] = a->value.setting.value;
			break;
		case ACTION_TFUNCTION:
			res = a->value.function.func(con, a->value.function.param);
			switch (res) {
			case ACTION_GO_ON:
			case ACTION_FINISHED:
				break;
			case ACTION_ERROR:
				action_stack_reset(con->srv, as);
				return res;
			case ACTION_WAIT_FOR_EVENT:
				return ACTION_WAIT_FOR_EVENT;
			}
			break;
		case ACTION_TCONDITION:
			if (condition_check(con, a->value.condition.cond)) {
				action_enter(con, a->value.condition.target);
			}
			else if (a->value.condition.target_else) {
				action_enter(con, a->value.condition.target_else);
			}
			break;
		case ACTION_TLIST:
			action_enter(con, a);
			break;
		}
		ase->pos++;
	}
	return ACTION_FINISHED;
}

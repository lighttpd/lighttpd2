
#include "actions.h"
#include "condition.h"

struct action_stack_element;
typedef struct action_stack_element action_stack_element;

struct action_stack_element {
	action_list *list;
	guint pos;
};



void action_release(action *a) {
	assert(a->refcount > 0);
	if (!(--a->refcount)) {
		switch (a->type) {
		case ACTION_TSETTING:
			/* TODO */
			break;
		case ACTION_TFUNCTION:
			/* TODO */
			break;
		case ACTION_TCONDITION:
			condition_release(a->value.condition.cond);
			action_list_release(a->value.condition.target);
			break;
		}
		g_slice_free(action, a);
	}
}

void action_acquire(action *a) {
	assert(a->refcount > 0);
	a->refcount++;
}

void action_list_release(action_list *al) {
	assert(al->refcount > 0);
	if (!(--al->refcount)) {
		guint i;
		for (i = al->actions->len; i-- > 0; ) {
			action_release(g_array_index(al->actions, action*, i));
		}
		g_array_free(al->actions, TRUE);
		g_slice_free(action_list, al);
	}
}

void action_list_acquire(action_list *al) {
	assert(al->refcount > 0);
	al->refcount++;
}

void action_stack_element_release(action_stack_element *ase) {
	if (!ase || !ase->list) return;
	action_list_release(ase->list);
	ase->list = NULL;
}

void action_stack_init(action_stack *as) {
	as->stack = g_array_sized_new(FALSE, TRUE, sizeof(action_stack_element), 15);
}

void action_stack_reset(action_stack *as) {
	guint i;
	for (i = as->stack->len; i-- > 0; ) {
		action_stack_element_release(&g_array_index(as->stack, action_stack_element, i));
	}
	g_array_set_size(as->stack, 0);
}

void action_stack_clear(action_stack *as) {
	guint i;
	for (i = as->stack->len; i-- > 0; ) {
		action_stack_element_release(&g_array_index(as->stack, action_stack_element, i));
	}
	g_array_free(as->stack, TRUE);
}

/** handle sublist now, remember current position (stack) */
void action_enter(connection *con, action_list *al) {
	action_list_acquire(al);
	action_stack_element ase = { al, 0 };
	g_array_append_val(con->action_stack.stack, ase);
}

static action_stack_element *action_stack_top(action_stack* as) {
	return as->stack->len > 0 ? &g_array_index(as->stack, action_stack_element, as->stack->len - 1) : NULL;
}

static void action_stack_pop(action_stack *as) {
	action_stack_element_release(&g_array_index(as->stack, action_stack_element, as->stack->len - 1));
	g_array_set_size(as->stack, as->stack->len - 1);
}

static action* action_stack_element_next(action_stack_element *ase) {
	action_list *al = ase->list;
	return ase->pos < al->actions->len ? g_array_index(al->actions, action*, ase->pos++) : NULL;
}

action_result action_execute(server *srv, connection *con) {
	action *a;
	action_stack *as = &con->action_stack;
	action_stack_element *ase;
	action_result res;

	while (NULL != (ase = action_stack_top(as))) {
		a = action_stack_element_next(ase);
		if (!a) {
			action_stack_pop(as);
			continue;
		}
		switch (a->type) {
		case ACTION_TSETTING:
			/* TODO */
			break;
		case ACTION_TFUNCTION:
			res = a->value.function.func(srv, con, a->value.function.param);
			switch (res) {
			case ACTION_GO_ON:
				break;
			case ACTION_FINISHED:
			case ACTION_ERROR:
				action_stack_clear(as);
				return res;
			case ACTION_WAIT_FOR_EVENT:
				return ACTION_WAIT_FOR_EVENT;
			}
			break;
		case ACTION_TCONDITION:
			if (condition_check(srv, con, a->value.condition.cond)) {
				action_enter(con, a->value.condition.target);
			}
			break;
		}
	}
	return ACTION_GO_ON;
}


#include "log.h"
#include "actions.h"
#include "condition.h"

struct action_stack_element;
typedef struct action_stack_element action_stack_element;

struct action_stack_element {
	action_list *list;
	guint pos;
};

action *action_new_setting(server *srv, const gchar *name, option *value) {
	option_set setting;
	if (!parse_option(srv, name, value, &setting)) {
		return NULL;
	}

	action *a = g_slice_new(action);

	a->refcount = 1;
	a->type = ACTION_TSETTING;
	a->value.setting = setting;

	return a;
}

action *action_new_function(ActionFunc func, ActionFree free, gpointer param) {
	action *a;

	a = g_slice_new(action);
	a->refcount = 1;
	a->type = ACTION_TFUNCTION;
	a->value.function.func = func;
	a->value.function.free = free;
	a->value.function.param = param;

	return a;
}

action *action_new_list() {
	action *a;

	a = g_slice_new(action);
	a->refcount = 1;
	a->type = ACTION_TLIST;
	a->value.list = action_list_new();

	return a;
}

action *action_new_condition(condition *cond, action_list *al) {
	action *a;

	a = g_slice_new(action);
	a->refcount = 1;
	a->type = ACTION_TCONDITION;
	a->value.condition.cond = cond;
	a->value.condition.target = al;

	return a;
}
void action_release(server *srv, action *a) {
	assert(a->refcount > 0);
	if (!(--a->refcount)) {
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
			action_list_release(srv, a->value.condition.target);
			break;
		case ACTION_TLIST:
			action_list_release(srv, a->value.list);
			break;
		}
		g_slice_free(action, a);
	}
}

void action_acquire(action *a) {
	assert(a->refcount > 0);
	a->refcount++;
}

action_list *action_list_new() {
	action_list *al;

	al = g_slice_new(action_list);
	al->refcount = 1;

	al->actions = g_array_new(FALSE, TRUE, sizeof(action *));

	return al;
}

action_list *action_list_from_action(action *a) {
	action_list *al;
	if (a->type == ACTION_TLIST) {
		action_list_acquire(a->value.list);
		return a->value.list; /* action gets released from lua */
	}
	action_acquire(a);
	al = action_list_new();
	g_array_append_val(al->actions, a);
	return al;
}

void action_list_release(server *srv, action_list *al) {
	assert(al->refcount > 0);
	if (!(--al->refcount)) {
		guint i;
		for (i = al->actions->len; i-- > 0; ) {
			action_release(srv, g_array_index(al->actions, action*, i));
		}
		g_array_free(al->actions, TRUE);
		g_slice_free(action_list, al);
	}
}

void action_list_acquire(action_list *al) {
	assert(al->refcount > 0);
	al->refcount++;
}

void action_stack_element_release(server *srv, action_stack_element *ase) {
	if (!ase || !ase->list) return;
	action_list_release(srv, ase->list);
	ase->list = NULL;
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

static void action_stack_pop(server *srv, action_stack *as) {
	action_stack_element_release(srv, &g_array_index(as->stack, action_stack_element, as->stack->len - 1));
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
			action_stack_pop(srv, as);
			continue;
		}
		switch (a->type) {
		case ACTION_TSETTING:
			con->options[a->value.setting.ndx] = a->value.setting.value;
			break;
		case ACTION_TFUNCTION:
			res = a->value.function.func(srv, con, a->value.function.param);
			switch (res) {
			case ACTION_GO_ON:
				break;
			case ACTION_FINISHED:
			case ACTION_ERROR:
				action_stack_clear(srv, as);
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
		case ACTION_TLIST:
			action_enter(con, a->value.list);
			break;
		}
	}
	return ACTION_GO_ON;
}

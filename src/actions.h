#ifndef _LIGHTTPD_ACTIONS_H_
#define _LIGHTTPD_ACTIONS_H_

#ifndef _LIGHTTPD_BASE_H_
#error Please include "base.h" instead of this file
#endif

/* action type */
typedef enum {
	ACTION_TSETTING,
	ACTION_TFUNCTION,
	ACTION_TCONDITION,
	ACTION_TLIST
} action_type;

struct action_stack {
	GArray* stack;
};

typedef handler_t (*ActionFunc)(vrequest *vr, gpointer param);
typedef void (*ActionFree)(server *srv, gpointer param);

struct action_func {
	ActionFunc func;
	ActionFree free;
	gpointer param;
};
typedef struct action_func action_func;

struct action {
	gint refcount;
	action_type type;

	union {
		option_set setting;

		struct {
			condition *cond;
			action *target; /** action target to jump to if condition is fulfilled */
			action *target_else; /** like above but if condition is not fulfilled */
		} condition;

		action_func function;

		GArray* list; /** array of (action*) */
	} data;
};

/* no new/free function, so just use the struct direct (i.e. not a pointer) */
LI_API void action_stack_init(action_stack *as);
LI_API void action_stack_reset(server *srv, action_stack *as);
LI_API void action_stack_clear(server *srv, action_stack *as);

/** handle sublist now, remember current position (stack) */
LI_API void action_enter(struct vrequest *vr, action *a);
LI_API handler_t action_execute(struct vrequest *vr);


LI_API void action_release(server *srv, action *a);
LI_API void action_acquire(action *a);
/* create new action */
LI_API action *action_new_setting(option_set setting);
LI_API action *action_new_function(ActionFunc func, ActionFree free, gpointer param);
LI_API action *action_new_list();
LI_API action *action_new_condition(condition *cond, action *target, action *target_else);

#endif

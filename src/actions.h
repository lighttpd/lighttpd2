#ifndef _LIGHTTPD_ACTIONS_H_
#define _LIGHTTPD_ACTIONS_H_

#include "settings.h"

typedef enum {
	ACTION_GO_ON,
	ACTION_FINISHED,
	ACTION_ERROR,
	ACTION_WAIT_FOR_EVENT
} action_result;

// action type
typedef enum {
	ACTION_TSETTING,
	ACTION_TFUNCTION,
	ACTION_TCONDITION,
	ACTION_TLIST
} action_type;

struct action;
typedef struct action action;

struct action_stack;
typedef struct action_stack action_stack;

struct action_stack {
	GArray* stack;
};

struct server; struct connection;
typedef action_result (*ActionFunc)(struct connection *con, gpointer param);
typedef void (*ActionFree)(struct server *srv, gpointer param);

struct action_func {
	ActionFunc func;
	ActionFree free;
	gpointer param;
};
typedef struct action_func action_func;

#include "condition.h"
#include "plugin.h"
#include "options.h"

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
	} value;
};

/* no new/free function, so just use the struct direct (i.e. not a pointer) */
LI_API void action_stack_init(action_stack *as);
LI_API void action_stack_reset(server *srv, action_stack *as);
LI_API void action_stack_clear(server *srv, action_stack *as);

/** handle sublist now, remember current position (stack) */
LI_API void action_enter(connection *con, action *a);
LI_API action_result action_execute(connection *con);


LI_API void action_release(server *srv, action *a);
LI_API void action_acquire(action *a);
/* create new action */
LI_API action *action_new_setting(option_set setting);
LI_API action *action_new_function(ActionFunc func, ActionFree free, gpointer param);
LI_API action *action_new_list();
LI_API action *action_new_condition(condition *cond, action *target, action *target_else);

#endif

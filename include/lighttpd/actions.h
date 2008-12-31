#ifndef _LIGHTTPD_ACTIONS_H_
#define _LIGHTTPD_ACTIONS_H_

#ifndef _LIGHTTPD_BASE_H_
#error Please include <lighttpd/base.h> instead of this file
#endif

/* action type */
typedef enum {
	ACTION_TSETTING,
	ACTION_TFUNCTION,
	ACTION_TCONDITION,
	ACTION_TLIST,
	ACTION_TBALANCER
} action_type;

typedef enum {
	BACKEND_OVERLOAD,
	BACKEND_DEAD
} backend_error;

struct action_stack {
	GArray* stack;
	gboolean backend_failed;
	backend_error backend_error;
};

/* param is the param registered with the callbacks;
 * in context the function can save extra data (like data for the stat-call)
 * If the context gets popped from the action stack and context is not zero,
 * the cleanup callback gets called.
 * you should not use *context without a cleanup callback!!!
 */
typedef handler_t (*ActionFunc)(vrequest *vr, gpointer param, gpointer *context);
typedef handler_t (*ActionCleanup)(vrequest *vr, gpointer param, gpointer context);
typedef void (*ActionFree)(server *srv, gpointer param);

struct action_func {
	ActionFunc func;
	ActionCleanup cleanup;
	ActionFree free;
	gpointer param;
};
typedef struct action_func action_func;


typedef handler_t (*BackendSelect)(vrequest *vr, gboolean backlog_provided, gpointer param, gpointer *context);
typedef handler_t (*BackendFallback)(vrequest *vr, gboolean backlog_provided, gpointer param, gpointer *context, backend_error error);
typedef handler_t (*BackendFinished)(vrequest *vr, gpointer param, gpointer context);
typedef void (*BalancerFree)(server *srv, gpointer param);
struct balancer_func {
	BackendSelect select;
	BackendFallback fallback;
	BackendFinished finished;
	BalancerFree free;
	gpointer param;
	gboolean provide_backlog;
};
typedef struct balancer_func balancer_func;


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

		balancer_func balancer;
	} data;
};

/* no new/free function, so just use the struct direct (i.e. not a pointer) */
LI_API void action_stack_init(action_stack *as);
LI_API void action_stack_reset(vrequest *vr, action_stack *as);
LI_API void action_stack_clear(vrequest *vr, action_stack *as);

/** handle sublist now, remember current position (stack) */
LI_API void action_enter(struct vrequest *vr, action *a);
LI_API handler_t action_execute(struct vrequest *vr);


LI_API void action_release(server *srv, action *a);
LI_API void action_acquire(action *a);
/* create new action */
LI_API action *action_new_setting(option_set setting);
LI_API action *action_new_function(ActionFunc func, ActionCleanup fcleanup, ActionFree ffree, gpointer param);
LI_API action *action_new_list();
LI_API action *action_new_condition(condition *cond, action *target, action *target_else);
LI_API action *action_new_balancer(BackendSelect bselect, BackendFallback bfallback, BackendFinished bfinished, BalancerFree bfree, gpointer param, gboolean provide_backlog);

#endif

#ifndef _LIGHTTPD_ACTIONS_H_
#define _LIGHTTPD_ACTIONS_H_

#ifndef _LIGHTTPD_BASE_H_
#error Please include <lighttpd/base.h> instead of this file
#endif

struct liActionRegexStackElement {
	GString *string;
	GMatchInfo *match_info;
};

struct liActionStack {
	GArray *stack, *regex_stack, *backend_stack;
	gboolean backend_failed, backend_finished;
	liBackendError backend_error;
};

/* param is the param registered with the callbacks;
 * in context the function can save extra data (like data for the stat-call)
 * If the context gets popped from the action stack and context is not zero,
 * the cleanup callback gets called.
 * you should not use *context without a cleanup callback!!!
 */
typedef liHandlerResult (*liActionFuncCB)(liVRequest *vr, gpointer param, gpointer *context);
typedef liHandlerResult (*liActionCleanupCB)(liVRequest *vr, gpointer param, gpointer context);
typedef void (*liActionFreeCB)(liServer *srv, gpointer param);

struct liActionFunc {
	liActionFuncCB func;
	liActionCleanupCB cleanup;
	liActionFreeCB free;
	gpointer param;
};


typedef liHandlerResult (*liBackendSelectCB)(liVRequest *vr, gboolean backlog_provided, gpointer param, gpointer *context);
typedef liHandlerResult (*liBackendFallbackCB)(liVRequest *vr, gboolean backlog_provided, gpointer param, gpointer *context, liBackendError error);
typedef liHandlerResult (*liBackendFinishedCB)(liVRequest *vr, gpointer param, gpointer context);
typedef void (*liBalancerFreeCB)(liServer *srv, gpointer param);

struct liBalancerFunc {
	liBackendSelectCB select;
	liBackendFallbackCB fallback;
	liBackendFinishedCB finished;
	liBalancerFreeCB free;
	gpointer param;
	gboolean provide_backlog;
};


struct liAction {
	gint refcount;
	liActionType type;

	union {
		liOptionSet setting;

		liOptionPtrSet settingptr;

		struct {
			liCondition *cond;
			liAction *target; /** action target to jump to if condition is fulfilled */
			liAction *target_else; /** like above but if condition is not fulfilled */
		} condition;

		liActionFunc function;

		GArray* list; /** array of (action*) */

		liBalancerFunc balancer;
	} data;
};

/* no new/free function, so just use the struct direct (i.e. not a pointer) */
LI_API void li_action_stack_init(liActionStack *as);
LI_API void li_action_stack_reset(liVRequest *vr, liActionStack *as);
LI_API void li_action_stack_clear(liVRequest *vr, liActionStack *as);

/** handle sublist now, remember current position (stack) */
LI_API void li_action_enter(liVRequest *vr, liAction *a);
LI_API liHandlerResult li_action_execute(liVRequest *vr);


LI_API void li_action_release(liServer *srv, liAction *a);
LI_API void li_action_acquire(liAction *a);
/* create new action */
LI_API liAction* li_action_new(void);
LI_API liAction* li_action_new_setting(liOptionSet setting);
LI_API liAction* li_action_new_settingptr(liOptionPtrSet setting);
LI_API liAction* li_action_new_function(liActionFuncCB func, liActionCleanupCB fcleanup, liActionFreeCB ffree, gpointer param);
LI_API liAction* li_action_new_list(void);
LI_API liAction* li_action_new_condition(liCondition *cond, liAction *target, liAction *target_else);
LI_API liAction* li_action_new_balancer(liBackendSelectCB bselect, liBackendFallbackCB bfallback, liBackendFinishedCB bfinished, liBalancerFreeCB bfree, gpointer param, gboolean provide_backlog);

/* assert(list->refcount == 1)! converts list to a list in place if necessary */
LI_API void li_action_append_inplace(liAction *list, liAction *element);

#endif

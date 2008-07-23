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
	ACTION_TCONDITION
} action_type;

struct action;
typedef struct action action;

struct action_list;
typedef struct action_list action_list;

struct action_stack;
typedef struct action_stack action_stack;

struct action_stack {
	GArray* stack;
};

struct server; struct connection;
typedef action_result (*ActionFunc)(struct server *srv, struct connection *con, gpointer param);
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

struct action_list {
	gint refcount;

	GArray* actions; /** array of (action*) */
};

struct action {
	gint refcount;
	action_type type;

	union {
		option_set setting;

		struct {
			condition *cond;
			action_list* target; /** action target to jump to if condition is fulfilled */
		} condition;

		action_func function;
	} value;
};

LI_API void action_list_release(action_list *al);
LI_API action_list *action_list_new();

/* no new/free function, so just use the struct direct (i.e. not a pointer) */
LI_API void action_stack_init(action_stack *as);
LI_API void action_stack_reset(action_stack *as);
LI_API void action_stack_clear(action_stack *as);

/** handle sublist now, remember current position (stack) */
LI_API void action_enter(connection *con, action_list *al);
LI_API action_result action_execute(server *srv, connection *con);


/* create new action */
LI_API action *action_new_setting(server *srv, GString *name, option *value);
LI_API action *action_new_function(server *srv, const char *name, option *value);
LI_API action *action_new_condition_string(comp_key_t comp, comp_operator_t op, GString *str);
LI_API action *action_new_condition_int(comp_key_t comp, comp_operator_t op, guint64 i);
#endif

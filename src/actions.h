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
typedef action_result (*action_func)(struct server *srv, struct connection *con, void* param);

#include "condition.h"

struct action_list {
	gint refcount;

	GArray* actions; /** array of (action*) */
};

struct action {
	gint refcount;
	action_type type;

	union {
		struct {
			GArray *options; /** array of option_mark */
		} setting;

		struct {
			condition *cond;
			action_list* target; /** action target to jump to if condition is fulfilled */
		} condition;

		struct {
			action_func func;
			gpointer param;
		} function;
	} value;
};

LI_API void action_list_release(action_list *al);

/* no new/free function, so just use the struct direct (i.e. not a pointer) */
LI_API void action_stack_init(action_stack *as);
LI_API void action_stack_reset(action_stack *as);
LI_API void action_stack_clear(action_stack *as);

/** handle sublist now, remember current position (stack) */
LI_API void action_enter(connection *con, action_list *al);
LI_API action_result action_execute(server *srv, connection *con);

#endif

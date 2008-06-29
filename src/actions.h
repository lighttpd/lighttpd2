#ifndef _LIGHTTPD_ACTIONS_H_
#define _LIGHTTPD_ACTIONS_H_

typedef enum { ACTION_RESULT_GO_ON, ACTION_RESULT_BREAK, ACTION_RESULT_WAIT_FOR_EVENT } action_result;

// action type
typedef enum { ACTION_SETTING, ACTION_FUNCTION, ACTION_CONDITION } action_type;

struct action;
typedef struct action action;

struct action_list;
typedef struct action_list action_list;

struct action_stack;
typedef struct action_stack action_stack;




struct action_list {
	GArray* actions;
	guint refcount;
};

struct action_stack {
	GArray* stack;
	guint index;
};

struct action_stack_elem {
	action_list* al;
	guint index;
};
typedef struct action_stack_elem action_stack_elem;

struct action {
	action_type type;

	union {
		struct {
			option_mark opt;
			option newvalue;
		} setting;

		condition cond;

		struct {
			action_func* func;
			gpointer param;
		} actionfunc;
	} value;
};

struct condition {
	condition_type type;
	condition_op op;
	action_list* target; // action target to jump to if condition is fulfilled

	// left value of condition
	union {
		guint val_int;
		gboolean val_bool;
		GString* val_string;
	} lvalue;

	// right value of condition
	union {
		guint val_int;
		gboolean val_bool;
		GString* val_string;
	} rvalue;
};

#endif

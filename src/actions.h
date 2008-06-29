#ifndef _LIGHTTPD_ACTIONS_H_
#define _LIGHTTPD_ACTIONS_H_

// action type
typedef enum { ACTION_SETTING, ACTION_FUNCTION, ACTION_CONDITION } action_type;

struct action;
typedef struct action action;

// condition operator
typedef enum
{
	CONDITION_EQUAL, CONDITION_UNEQUAL,
	CONDITION_LESS, CONDITION_LESS_EQUAL,
	CONDITION_GREATER, CONDITION_GREATER_EQUAL,
	CONDITION_REGEX_MATCH, CONDITION_REGEX_NOMATCH
} condition_op;

// condition type
typedef enum { CONDITION_BOOL, CONDITION_INT, CONDITION_STRING, CONDITION_IP } condition_type;

struct condition;
typedef struct condition condition;


struct action
{
	action_type type;

	union
	{
		struct
		{
			option_mark opt;
			option newvalue;
		} setting;

		condition cond;

		struct
		{
			action_func* func;
			gpointer param;
		} actionfunc;
	} value;

	action* next;
};

struct condition
{
	condition_type type;
	condition_op op;
	action* target; // action target to jump to if condition is fulfilled

	// left value of condition
	union
	{
		guint val_int;
		gboolean val_bool;
		GString* val_string;
	} lvalue;

	// right value of condition
	union
	{
		guint val_int;
		gboolean val_bool;
		GString* val_string;
	} rvalue;
};

#endif

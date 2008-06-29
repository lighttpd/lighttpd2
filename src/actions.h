#ifndef _LIGHTTPD_ACTIONS_H_
#define _LIGHTTPD_ACTIONS_H_

typedef enum { ACTION_SETTING, ACTION_FUNCTION, ACTION_CONDITION } action_type;

struct action;
typedef struct action action;

typedef enum
{
	CONDITION_EQUAL, CONDITION_UNEQUAL,
	CONDITION_LESS, CONDITION_LESS_EQUAL,
	CONDITION_GREATER, CONDITION_GREATER_EQUAL,
	CONDITION_REGEX_MATCH, CONDITION_REGEX_NOMATCH
} condition_op;

typedef enum { CONDITION_BOOL, CONDITION_INT, CONDITION_STRING, CONDITION_IP } condition_type;

struct condition;
typedef struct condition condition;


struct action
{
	action_type type;

	union
	{
		option param;
		condition cond;
	} value;

	action* next;
};

struct condition
{
	condition_type type;
	condition_op op;

	union
	{
		guint val_int;
		gboolean val_bool;
		GString* val_string;
	} lvalue;

	union
	{
		guint val_int;
		gboolean val_bool;
		GString* val_string;
	} rvalue;
};

#endif

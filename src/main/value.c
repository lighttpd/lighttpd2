#include <lighttpd/base.h>

liValue* li_value_new_action(liServer *srv, liAction *a) {
	liValue *v = g_slice_new0(liValue);
	v->data.val_action.srv = srv;
	v->data.val_action.action = a;
	v->type = LI_VALUE_ACTION;
	return v;
}

liValue* li_value_new_condition(liServer *srv, liCondition *c) {
	liValue *v = g_slice_new0(liValue);
	v->data.val_cond.srv = srv;
	v->data.val_cond.cond = c;
	v->type = LI_VALUE_CONDITION;
	return v;
}

liValue* li_value_copy(liValue* val) {
	liValue *n;
	if (NULL == val) return NULL;

	switch (val->type) {
	case LI_VALUE_ACTION:
		li_action_acquire(val->data.val_action.action);
		n = li_value_new_action(val->data.val_action.srv, val->data.val_action.action);
		return n;
	case LI_VALUE_CONDITION:
		li_condition_acquire(val->data.val_cond.cond);
		n = li_value_new_condition(val->data.val_cond.srv, val->data.val_cond.cond);
		return n;
	default:
		return li_common_value_copy_(val);
	}
	return NULL;
}

static void _li_value_clear(liValue *val) {
	memset(val, 0, sizeof(*val));
	val->type = LI_VALUE_NONE;
}

void li_value_clear(liValue *val) {
	if (NULL == val) return;

	switch (val->type) {
	case LI_VALUE_ACTION:
		li_action_release(val->data.val_action.srv, val->data.val_action.action);
		_li_value_clear(val);
		break;
	case LI_VALUE_CONDITION:
		li_condition_release(val->data.val_cond.srv, val->data.val_cond.cond);
		_li_value_clear(val);
		break;
	default:
		li_common_value_clear_(val);
	}
}

const char* li_valuetype_string(liValueType type) {
	switch(type) {
	case LI_VALUE_ACTION:
		return "action";
	case LI_VALUE_CONDITION:
		return "condition";
	default:
		return li_common_valuetype_string_(type);
	}
}

GString *li_value_to_string(liValue *val) {
	switch (val->type) {
	case LI_VALUE_ACTION:
		return g_string_new_len(CONST_STR_LEN("<action>"));
	case LI_VALUE_CONDITION:
		return g_string_new_len(CONST_STR_LEN("<condition>"));
	default:
		return li_common_value_to_string_(val);
	}
}

gpointer li_value_extract_ptr(liValue *val) {
	gpointer ptr;

	if (NULL == val) return NULL;

	switch (val->type) {
	case LI_VALUE_ACTION:
		ptr = val->data.val_action.action;
		_li_value_clear(val);
		return ptr;
	case LI_VALUE_CONDITION:
		ptr = val->data.val_action.action;
		_li_value_clear(val);
		return ptr;
	default:
		return li_common_value_extract_ptr_(val);
	}
}

liAction* li_value_extract_action(liValue *val) {
	liAction* result;
	if (NULL == val || val->type != LI_VALUE_ACTION) return NULL;
	result = val->data.val_action.action;
	_li_value_clear(val);
	return result;
}

liCondition* li_value_extract_condition(liValue *val) {
	liCondition* result;
	if (NULL == val || val->type != LI_VALUE_CONDITION) return NULL;
	result = val->data.val_cond.cond;
	_li_value_clear(val);
	return result;
}

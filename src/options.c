
#include <lighttpd/base.h>

/* Extract ovalue from ovalue, ovalue set to none */
option_value value_extract(value *val) {
	option_value oval = {0};
	if (!val) return oval;

	switch (val->type) {
		case VALUE_NONE:
			break;
		case VALUE_BOOLEAN:
			oval.boolean = val->data.boolean;
			break;
		case VALUE_NUMBER:
			oval.number =  val->data.number;
			break;
		case VALUE_STRING:
			oval.string = val->data.string;
			break;
		case VALUE_LIST:
			oval.list =  val->data.list;
			break;
		case VALUE_HASH:
			oval.hash =  val->data.hash;
			break;
		case VALUE_ACTION:
			oval.ptr = val->data.val_action.action;
			break;
		case VALUE_CONDITION:
			oval.ptr = val->data.val_action.action;
			break;
	}
	val->type = VALUE_NONE;
	return oval;
}

gpointer value_extract_ptr(value *val) {
	option_value oval = value_extract(val);
	return oval.ptr;
}
gint64 value_extract_number(value *val) {
	option_value oval = value_extract(val);
	return oval.number;
}
gboolean value_extract_bool(value *val) {
	option_value oval = value_extract(val);
	return oval.boolean;
}

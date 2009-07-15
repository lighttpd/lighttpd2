
#include <lighttpd/base.h>

/* Extract ovalue from ovalue, ovalue set to none */
liOptionValue li_value_extract(liValue *val) {
	liOptionValue oval = {0};
	if (!val) return oval;

	switch (val->type) {
		case LI_VALUE_NONE:
			break;
		case LI_VALUE_BOOLEAN:
			oval.boolean = val->data.boolean;
			break;
		case LI_VALUE_NUMBER:
			oval.number =  val->data.number;
			break;
		case LI_VALUE_STRING:
			oval.string = val->data.string;
			break;
		case LI_VALUE_LIST:
			oval.list =  val->data.list;
			break;
		case LI_VALUE_HASH:
			oval.hash =  val->data.hash;
			break;
		case LI_VALUE_ACTION:
			oval.ptr = val->data.val_action.action;
			break;
		case LI_VALUE_CONDITION:
			oval.ptr = val->data.val_action.action;
			break;
	}
	val->type = LI_VALUE_NONE;
	return oval;
}

gpointer li_value_extract_ptr(liValue *val) {
	liOptionValue oval = li_value_extract(val);
	return oval.ptr;
}
gint64 li_value_extract_number(liValue *val) {
	liOptionValue oval = li_value_extract(val);
	return oval.number;
}
gboolean li_value_extract_bool(liValue *val) {
	liOptionValue oval = li_value_extract(val);
	return oval.boolean;
}

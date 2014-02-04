#include <lighttpd/angel_base.h>

liValue* li_value_copy(liValue* val) {
	return li_common_value_copy_(val);
}

void li_value_clear(liValue *val) {
	li_common_value_clear_(val);
}

const char* li_valuetype_string(liValueType type) {
	return li_common_valuetype_string_(type);
}

GString *li_value_to_string(liValue *val) {
	return li_common_value_to_string_(val);
}

gpointer li_value_extract_ptr(liValue *val) {
	return li_common_value_extract_ptr_(val);
}

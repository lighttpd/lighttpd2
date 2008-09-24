
#include "base.h"

void string_destroy_notify(gpointer *str) {
	g_string_free((GString*)str, TRUE);
}

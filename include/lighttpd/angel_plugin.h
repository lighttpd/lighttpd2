#ifndef _LIGHTTPD_ANGEL_PLUGIN_H_
#define _LIGHTTPD_ANGEL_PLUGIN_H_

#ifndef _LIGHTTPD_ANGEL_BASE_H_
#error Please include <lighttpd/angel_base.h> instead of this file
#endif


LI_API gboolean plugins_handle_item(server *srv, value *hash);

#endif

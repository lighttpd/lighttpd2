#ifndef _LIGHTTPD_CONNECTION_H_
#define _LIGHTTPD_CONNECTION_H_

struct connection {

	sock_addr remote_addr, local_addr;
	GString *remote_addr_str, *local_addr_str;
	gboolean is_ssl;

	action_stack action_stack;

	gpointer *options;

	request request;
	physical physical;

	GMutex *mutex;

	struct log_t *log;
	gint log_level;
};

#endif

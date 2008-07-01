
#include "base.h"

static server_option* find_option(server *srv, const char *key) {
	return (server_option*) g_hash_table_lookup(srv->options, key);
}

gboolean parse_option(server *srv, const char *key, option *opt, option_mark *mark) {
	server_option *sopt;

	if (!srv || !key || !mark) return FALSE;

	sopt = find_option(srv, key);
	if (!sopt) return FALSE;

	/* TODO */
	UNUSED(opt);

	return FALSE;
}

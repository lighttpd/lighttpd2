
bin_PROGRAMS=lighttpd
common_cflags=-I$(top_srcdir)/include -I$(top_builddir)/include

lighttpd_shared_src= \
	angel.c \
	angel_fake.c \
	actions.c \
	chunk.c \
	chunk_parser.c \
	collect.c \
	condition.c \
	config_parser.c \
	connection.c \
	environment.c \
	etag.c \
	filter_chunked.c \
	http_headers.c \
	http_request_parser.c \
	http_response_parser.c \
	lighttpd-glue.c \
	log.c \
	network.c \
	network_write.c network_writev.c \
	network_sendfile.c \
	options.c \
	plugin.c \
	profiler.c \
	request.c \
	response.c \
	server.c \
	stat_cache.c \
	throttle.c \
	url_parser.c \
	value.c \
	virtualrequest.c \
	worker.c \
	 \
	plugin_core.c

if USE_LUA
lighttpd_shared_src+= \
	actions_lua.c \
	condition_lua.c \
	config_lua.c \
	value_lua.c
endif
EXTRA_lighttpd_SOURCES=\
	actions_lua.c \
	condition_lua.c \
	config_lua.c \
	value_lua.c

BUILT_SOURCES=config_parser.c http_request_parser.c http_response_parser.c url_parser.c

config_parser.c: config_parser.rl
	ragel -C -T0 -o $@ $<
http_request_parser.c: http_request_parser.rl
	ragel -C -T1 -o $@ $<
http_response_parser.c: http_response_parser.rl
	ragel -C -T1 -o $@ $<
url_parser.c: url_parser.rl
	ragel -C -T1 -o $@ $<

lighttpd_SOURCES=lighttpd.c $(lighttpd_shared_src)

lighttpd_CPPFLAGS=$(common_cflags) $(GTHREAD_CFLAGS) $(GMODULE_CFLAGS) $(LIBEV_CFLAGS) $(LUA_CFLAGS) -DDEFAULT_LIBDIR='"$(pkglibdir)"'
lighttpd_LDFLAGS=-export-dynamic $(GTHREAD_LIBS) $(GMODULE_LIBS) $(LIBEV_LIBS) $(LUA_LIBS)
lighttpd_LDADD=../common/liblighttpd-common.la

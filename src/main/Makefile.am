
bin_PROGRAMS=lighttpd
lib_LTLIBRARIES=liblighttpd-shared.la

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
	filter_buffer_on_disk.c \
	http_headers.c \
	http_range_parser.c \
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

lua_src= \
	actions_lua.c \
	condition_lua.c \
	config_lua.c \
	value_lua.c \
	 \
	chunk_lua.c \
	connection_lua.c \
	core_lua.c \
	environment_lua.c \
	filters_lua.c \
	http_headers_lua.c \
	physical_lua.c \
	request_lua.c \
	response_lua.c \
	stat_lua.c \
	virtualrequest_lua.c


if USE_LUA
lighttpd_shared_src+=$(lua_src)
endif
EXTRA_lighttpd_SOURCES=$(lua_src)

BUILT_SOURCES=config_parser.c http_range_parser.c http_request_parser.c http_response_parser.c url_parser.c

config_parser.c: config_parser.rl
	ragel -C -T0 -o $@ $<
http_range_parser.c: http_range_parser.rl
	ragel -C -T1 -o $@ $<
http_request_parser.c: http_request_parser.rl
	ragel -C -T1 -o $@ $<
http_response_parser.c: http_response_parser.rl
	ragel -C -T1 -o $@ $<
url_parser.c: url_parser.rl
	ragel -C -T1 -o $@ $<

EXTRA_DIST=config_parser.rl http_range_parser.rl http_request_parser.rl  http_response_parser.rl url_parser.rl

liblighttpd_shared_la_SOURCES=$(lighttpd_shared_src)
liblighttpd_shared_la_CPPFLAGS=$(common_cflags) $(GTHREAD_CFLAGS) $(GMODULE_CFLAGS) $(LIBEV_CFLAGS) $(LUA_CFLAGS)
liblighttpd_shared_la_LDFLAGS=-export-dynamic $(GTHREAD_LIBS) $(GMODULE_LIBS) $(LIBEV_LIBS)

lighttpd_SOURCES=lighttpd.c

lighttpd_CPPFLAGS=$(common_cflags) $(GTHREAD_CFLAGS) $(GMODULE_CFLAGS) $(LIBEV_CFLAGS) $(LUA_CFLAGS) -DDEFAULT_LIBDIR='"$(pkglibdir)"'
lighttpd_LDFLAGS=-export-dynamic $(GTHREAD_LIBS) $(GMODULE_LIBS) $(LIBEV_LIBS) $(LUA_LIBS)
lighttpd_LDADD=../common/liblighttpd-common.la ../main/liblighttpd-shared.la

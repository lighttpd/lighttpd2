
libexec_PROGRAMS=lighttpd2-worker
lib_LTLIBRARIES=liblighttpd2-shared.la

common_cflags=-I$(top_builddir)/include -I$(top_srcdir)/include

lighttpd_shared_src= \
	angel.c \
	angel_fake.c \
	actions.c \
	base_lua.c \
	backends.c \
	chunk.c \
	chunk_parser.c \
	collect.c \
	condition.c \
	config_parser.c \
	connection.c \
	environment.c \
	etag.c \
	filter.c \
	filter_chunked.c \
	filter_buffer_on_disk.c \
	http_headers.c \
	http_range_parser.c \
	http_request_parser.c \
	http_response_parser.c \
	lighttpd_glue.c \
	log.c \
	mimetype.c \
	network.c \
	network_write.c network_writev.c \
	network_sendfile.c \
	options.c \
	pattern.c \
	plugin.c \
	request.c \
	response.c \
	server.c \
	stat_cache.c \
	stream.c \
	stream_http_response.c \
	stream_simple_socket.c \
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
	core_lua.c \
	environment_lua.c \
	filters_lua.c \
	http_headers_lua.c \
	physical_lua.c \
	request_lua.c \
	response_lua.c \
	stat_lua.c \
	subrequest_lua.c \
	virtualrequest_lua.c


if USE_LUA
lighttpd_shared_src+=$(lua_src)
endif

BUILT_SOURCES=config_parser.c http_range_parser.c http_request_parser.c http_response_parser.c url_parser.c

config_parser.c: config_parser.rl
	$(RAGEL) -C -T0 -o $@ $<
http_range_parser.c: http_range_parser.rl
	$(RAGEL) -C -T1 -o $@ $<
http_request_parser.c: http_request_parser.rl
	$(RAGEL) -C -T1 -o $@ $<
http_response_parser.c: http_response_parser.rl
	$(RAGEL) -C -T1 -o $@ $<
url_parser.c: url_parser.rl
	$(RAGEL) -C -T1 -o $@ $<

EXTRA_DIST=config_parser.rl http_range_parser.rl http_request_parser.rl  http_response_parser.rl url_parser.rl

liblighttpd2_shared_la_SOURCES=$(lighttpd_shared_src)
liblighttpd2_shared_la_CPPFLAGS=$(common_cflags) $(GTHREAD_CFLAGS) $(GMODULE_CFLAGS) $(LIBEV_CFLAGS) $(LUA_CFLAGS)
liblighttpd2_shared_la_LDFLAGS=-release $(PACKAGE_VERSION) -export-dynamic $(GTHREAD_LIBS) $(GMODULE_LIBS) $(LIBEV_LIBS) $(LUA_LIBS)
liblighttpd2_shared_la_LIBADD=../common/liblighttpd2-common.la

lighttpd2_worker_SOURCES=lighttpd_worker.c

lighttpd2_worker_CPPFLAGS=$(common_cflags) $(GTHREAD_CFLAGS) $(GMODULE_CFLAGS) $(LIBEV_CFLAGS) $(LUA_CFLAGS) -DDEFAULT_LIBDIR='"$(pkglibdir)"'
lighttpd2_worker_LDFLAGS=-export-dynamic $(GTHREAD_LIBS) $(GMODULE_LIBS) $(LIBEV_LIBS) $(LUA_LIBS)
lighttpd2_worker_LDADD=../common/liblighttpd2-common.la liblighttpd2-shared.la


lib_LTLIBRARIES=liblighttpd2-common.la
common_cflags=-I$(top_builddir)/include -I$(top_srcdir)/include

common_src= \
	angel_connection.c \
	angel_data.c \
	buffer.c \
	encoding.c \
	events.c \
	fetch.c \
	idlist.c \
	ip_parsers.c \
	jobqueue.c \
	memcached.c \
	mempool.c \
	module.c \
	radix.c \
	sys_memory.c \
	sys_socket.c \
	tasklet.c \
	utils.c \
	waitqueue.c

if WITH_PROFILER
common_src+= profiler.c
endif
EXTRA_liblighttpd2_common_la_SOURCES=$(lua_src) profiler.c

BUILT_SOURCES=ip_parsers.c

ip_parsers.c: ip_parsers.rl
	$(RAGEL) -C -T1 -o $@ $<

EXTRA_DIST=ip_parsers.rl

liblighttpd2_common_la_SOURCES=$(common_src)
liblighttpd2_common_la_CPPFLAGS=$(common_cflags) $(GTHREAD_CFLAGS) $(GMODULE_CFLAGS) $(LIBEV_CFLAGS)
liblighttpd2_common_la_LDFLAGS=-release $(PACKAGE_VERSION) -export-dynamic $(GTHREAD_LIBS) $(GMODULE_LIBS) $(LIBEV_LIBS) $(CRYPT_LIB)

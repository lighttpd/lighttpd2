
lib_LTLIBRARIES=liblighttpd2-common.la
common_cflags=-I$(top_srcdir)/include -I$(top_builddir)/include

common_src= \
	angel_connection.c \
	angel_data.c \
	buffer.c \
	encoding.c \
	idlist.c \
	ip_parsers.c \
	mempool.c \
	module.c \
	radix.c \
	sys-files.c \
	sys-socket.c \
	utils.c \
	waitqueue.c

BUILT_SOURCES=ip_parsers.c

ip_parsers.c: ip_parsers.rl
	ragel -C -T1 -o $@ $<

EXTRA_DIST=ip_parsers.rl

liblighttpd2_common_la_SOURCES=$(common_src)
liblighttpd2_common_la_CPPFLAGS=$(common_cflags) $(GTHREAD_CFLAGS) $(GMODULE_CFLAGS) $(LIBEV_CFLAGS)
liblighttpd2_common_la_LDFLAGS=-release $(PACKAGE_VERSION) -export-dynamic $(GTHREAD_LIBS) $(GMODULE_LIBS) $(LIBEV_LIBS)

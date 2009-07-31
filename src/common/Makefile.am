
noinst_LIBRARIES=libcommon.a
common_cflags=-I$(top_srcdir)/include -I$(top_builddir)/include

common_src= \
	angel_connection.c \
	angel_data.c \
	encoding.c \
	idlist.c \
	ip_parsers.c \
	module.c \
	radix.c \
	sys-files.c \
	sys-socket.c \
	utils.c \
	waitqueue.c

BUILT_SOURCES=ip_parsers.c

ip_parsers.c: ip_parsers.rl
	ragel -C -T1 -o $@ $<

libcommon_a_SOURCES=$(common_src)
libcommon_a_CPPFLAGS=$(common_cflags) $(GTHREAD_CFLAGS) $(GMODULE_CFLAGS) $(LIBEV_CFLAGS) $(LUA_CFLAGS)

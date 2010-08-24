
sbin_PROGRAMS=lighttpd2
lib_LTLIBRARIES=liblighttpd2-sharedangel.la

common_cflags=-I$(top_srcdir)/include -I$(top_builddir)/include

lighttpd_angel_shared_src= \
	angel_config_parser.c \
	angel_log.c \
	angel_plugin.c \
	angel_plugin_core.c \
	angel_proc.c \
	angel_server.c \
	angel_value.c

BUILT_SOURCES=angel_config_parser.c

angel_config_parser.c: angel_config_parser.rl
	ragel -C -T1 -o $@ $<

EXTRA_DIST=angel_config_parser.rl

liblighttpd2_sharedangel_la_SOURCES=$(lighttpd_angel_shared_src)
liblighttpd2_sharedangel_la_CPPFLAGS=$(common_cflags) $(GTHREAD_CFLAGS) $(GMODULE_CFLAGS) $(LIBEV_CFLAGS) $(LUA_CFLAGS)
liblighttpd2_sharedangel_la_LDFLAGS=-release $(PACKAGE_VERSION) -export-dynamic $(GTHREAD_LIBS) $(GMODULE_LIBS) $(LIBEV_LIBS)
liblighttpd2_sharedangel_la_LIBADD=../common/liblighttpd2-common.la

lighttpd2_SOURCES=angel_main.c

lighttpd2_CPPFLAGS=$(common_cflags) $(GTHREAD_CFLAGS) $(GMODULE_CFLAGS) $(LIBEV_CFLAGS) $(LUA_CFLAGS) -DDEFAULT_LIBDIR='"$(pkglibdir)"' -DDEFAULT_LIBEXECDIR='"$(libexecdir)"'
lighttpd2_LDFLAGS=-export-dynamic $(GTHREAD_LIBS) $(GMODULE_LIBS) $(LIBEV_LIBS) $(LUA_LIBS)
lighttpd2_LDADD=../common/liblighttpd2-common.la liblighttpd2-sharedangel.la

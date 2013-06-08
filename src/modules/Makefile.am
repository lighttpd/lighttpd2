install_libs =

common_cflags = -I$(top_srcdir)/include -I$(top_builddir)/include
common_cflags += $(GTHREAD_CFLAGS) $(LIBEV_CFLAGS) $(LUA_CFLAGS)
common_libs = $(GTHREAD_LIBS) $(LIBEV_LIBS) $(LUA_LIBS)
common_ldflags = -module -export-dynamic -avoid-version -no-undefined $(common_libs)
common_libadd = ../common/liblighttpd2-common.la ../main/liblighttpd2-shared.la
EXTRA_DIST=ssl-session-db.h

luadir = $(datarootdir)/lighttpd2/lua

AM_CPPFLAGS = $(common_cflags)

install_libs += libmod_access.la
libmod_access_la_SOURCES = mod_access.c
libmod_access_la_LDFLAGS = $(common_ldflags)
libmod_access_la_LIBADD = $(common_libadd)

install_libs += libmod_accesslog.la
libmod_accesslog_la_SOURCES = mod_accesslog.c
libmod_accesslog_la_LDFLAGS = $(common_ldflags)
libmod_accesslog_la_LIBADD = $(common_libadd)

install_libs += libmod_auth.la
libmod_auth_la_SOURCES = mod_auth.c
libmod_auth_la_LDFLAGS = $(common_ldflags)
libmod_auth_la_LIBADD = $(common_libadd)

install_libs += libmod_balance.la
libmod_balance_la_SOURCES = mod_balance.c
libmod_balance_la_LDFLAGS = $(common_ldflags)
libmod_balance_la_LIBADD = $(common_libadd)

install_libs += libmod_cache_disk_etag.la
libmod_cache_disk_etag_la_SOURCES = mod_cache_disk_etag.c
libmod_cache_disk_etag_la_LDFLAGS = $(common_ldflags)
libmod_cache_disk_etag_la_LIBADD = $(common_libadd)

install_libs += libmod_debug.la
libmod_debug_la_SOURCES = mod_debug.c
libmod_debug_la_LDFLAGS = $(common_ldflags)
libmod_debug_la_LIBADD = $(common_libadd)

if USE_MOD_DEFLATE
install_libs += libmod_deflate.la
libmod_deflate_la_SOURCES = mod_deflate.c
libmod_deflate_la_LDFLAGS = $(common_ldflags)
libmod_deflate_la_LIBADD = $(common_libadd) $(Z_LIB) $(BZ_LIB)
endif

install_libs += libmod_dirlist.la
libmod_dirlist_la_SOURCES = mod_dirlist.c
libmod_dirlist_la_LDFLAGS = $(common_ldflags)
libmod_dirlist_la_LIBADD = $(common_libadd)

install_libs += libmod_expire.la
libmod_expire_la_SOURCES = mod_expire.c
libmod_expire_la_LDFLAGS = $(common_ldflags)
libmod_expire_la_LIBADD = $(common_libadd)

install_libs += libmod_fastcgi.la
libmod_fastcgi_la_SOURCES = mod_fastcgi.c fastcgi_stream.c
libmod_fastcgi_la_LDFLAGS = $(common_ldflags)
libmod_fastcgi_la_LIBADD = $(common_libadd)
EXTRA_DIST += fastcgi_stream.h

install_libs += libmod_flv.la
libmod_flv_la_SOURCES = mod_flv.c
libmod_flv_la_LDFLAGS = $(common_ldflags)
libmod_flv_la_LIBADD = $(common_libadd)

install_libs += libmod_fortune.la
libmod_fortune_la_SOURCES = mod_fortune.c
libmod_fortune_la_LDFLAGS = $(common_ldflags)
libmod_fortune_la_LIBADD = $(common_libadd)

if USE_GNUTLS
install_libs += libmod_gnutls.la
libmod_gnutls_la_CPPFLAGS = $(AM_CPPFLAGS) $(GNUTLS_CFLAGS) $(IDN_FLAGS)
libmod_gnutls_la_SOURCES = mod_gnutls.c gnutls_filter.c
libmod_gnutls_la_LDFLAGS = $(common_ldflags)
libmod_gnutls_la_LIBADD = $(common_libadd) $(GNUTLS_LIBS) $(IDN_LIBS)
endif
EXTRA_DIST += gnutls_filter.h

install_libs += libmod_limit.la
libmod_limit_la_SOURCES = mod_limit.c
libmod_limit_la_LDFLAGS = $(common_ldflags)
libmod_limit_la_LIBADD = $(common_libadd)

if USE_LUA
install_libs += libmod_lua.la
libmod_lua_la_CPPFLAGS = $(AM_CPPFLAGS) -DDEFAULT_LUADIR='"$(luadir)"'
libmod_lua_la_SOURCES = mod_lua.c
libmod_lua_la_LDFLAGS = $(common_ldflags)
libmod_lua_la_LIBADD = $(common_libadd)
endif

install_libs += libmod_memcached.la
libmod_memcached_la_SOURCES = mod_memcached.c
libmod_memcached_la_LDFLAGS = $(common_ldflags)
libmod_memcached_la_LIBADD = $(common_libadd)

if USE_OPENSSL
install_libs += libmod_openssl.la
libmod_openssl_la_CPPFLAGS =  $(AM_CPPFLAGS) $(OPENSSL_CFLAGS) $(IDN_FLAGS)
libmod_openssl_la_SOURCES = mod_openssl.c openssl_filter.c
libmod_openssl_la_LDFLAGS = $(common_ldflags)
libmod_openssl_la_LIBADD = $(common_libadd) $(OPENSSL_LIBS) $(IDN_LIBS)
endif
EXTRA_DIST += openssl_filter.h

install_libs += libmod_progress.la
libmod_progress_la_SOURCES = mod_progress.c
libmod_progress_la_LDFLAGS = $(common_ldflags)
libmod_progress_la_LIBADD = $(common_libadd)

install_libs += libmod_proxy.la
libmod_proxy_la_SOURCES = mod_proxy.c
libmod_proxy_la_LDFLAGS = $(common_ldflags)
libmod_proxy_la_LIBADD = $(common_libadd)

install_libs += libmod_redirect.la
libmod_redirect_la_SOURCES = mod_redirect.c
libmod_redirect_la_LDFLAGS = $(common_ldflags)
libmod_redirect_la_LIBADD = $(common_libadd)

install_libs += libmod_rewrite.la
libmod_rewrite_la_SOURCES = mod_rewrite.c
libmod_rewrite_la_LDFLAGS = $(common_ldflags)
libmod_rewrite_la_LIBADD = $(common_libadd)

install_libs += libmod_scgi.la
libmod_scgi_la_SOURCES = mod_scgi.c
libmod_scgi_la_LDFLAGS = $(common_ldflags)
libmod_scgi_la_LIBADD = $(common_libadd)

install_libs += libmod_status.la
libmod_status_la_SOURCES = mod_status.c
libmod_status_la_LDFLAGS = $(common_ldflags)
libmod_status_la_LIBADD = $(common_libadd)

install_libs += libmod_throttle.la
libmod_throttle_la_SOURCES = mod_throttle.c
libmod_throttle_la_LDFLAGS = $(common_ldflags)
libmod_throttle_la_LIBADD = $(common_libadd)

install_libs += libmod_userdir.la
libmod_userdir_la_SOURCES = mod_userdir.c
libmod_userdir_la_LDFLAGS = $(common_ldflags)
libmod_userdir_la_LIBADD = $(common_libadd)

install_libs += libmod_vhost.la
libmod_vhost_la_SOURCES = mod_vhost.c
libmod_vhost_la_LDFLAGS = $(common_ldflags)
libmod_vhost_la_LIBADD = $(common_libadd)

pkglib_LTLIBRARIES=$(install_libs)

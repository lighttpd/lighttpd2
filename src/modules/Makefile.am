install_libs = 

common_cflags = -I$(top_srcdir)/include -I$(top_builddir)/include
common_cflags += $(GTHREAD_CFLAGS) $(GMODULE_CFLAGS) $(LIBEV_CFLAGS) $(LUA_CFLAGS)
common_libadd = $(GTHREAD_LIBS) $(GMODULE_LIBS) $(LIBEV_LIBS) $(LUA_LIBS)
common_ldflags = -module -export-dynamic -avoid-version -no-undefined $(common_libadd)

AM_CPPFLAGS = $(common_cflags)

install_libs += mod_access.la
mod_access_la_SOURCES = mod_access.c
mod_access_la_LDFLAGS = -module -export-dynamic -avoid-version -no-undefined

install_libs += mod_accesslog.la
mod_accesslog_la_SOURCES = mod_accesslog.c
mod_accesslog_la_LDFLAGS = -module -export-dynamic -avoid-version -no-undefined

install_libs += mod_balancer.la
mod_balancer_la_SOURCES = mod_balancer.c
mod_balancer_la_LDFLAGS = -module -export-dynamic -avoid-version -no-undefined

install_libs += mod_cache_disk_etag.la
mod_cache_disk_etag_la_SOURCES = mod_cache_disk_etag.c
mod_cache_disk_etag_la_LDFLAGS = -module -export-dynamic -avoid-version -no-undefined

install_libs += mod_debug.la
mod_debug_la_SOURCES = mod_debug.c
mod_debug_la_LDFLAGS = -module -export-dynamic -avoid-version -no-undefined

install_libs += mod_dirlist.la
mod_dirlist_la_SOURCES = mod_dirlist.c
mod_dirlist_la_LDFLAGS = -module -export-dynamic -avoid-version -no-undefined

install_libs += mod_expire.la
mod_expire_la_SOURCES = mod_expire.c
mod_expire_la_LDFLAGS = -module -export-dynamic -avoid-version -no-undefined

install_libs += mod_fastcgi.la
mod_fastcgi_la_SOURCES = mod_fastcgi.c
mod_fastcgi_la_LDFLAGS = -module -export-dynamic -avoid-version -no-undefined

install_libs += mod_fortune.la
mod_fortune_la_SOURCES = mod_fortune.c
mod_fortune_la_LDFLAGS = -module -export-dynamic -avoid-version -no-undefined

install_libs += mod_redirect.la
mod_redirect_la_SOURCES = mod_redirect.c
mod_redirect_la_LDFLAGS = -module -export-dynamic -avoid-version -no-undefined

install_libs += mod_rewrite.la
mod_rewrite_la_SOURCES = mod_rewrite.c
mod_rewrite_la_LDFLAGS = -module -export-dynamic -avoid-version -no-undefined

install_libs += mod_status.la
mod_status_la_SOURCES = mod_status.c
mod_status_la_LDFLAGS = -module -export-dynamic -avoid-version -no-undefined

install_libs += mod_vhost.la
mod_vhost_la_SOURCES = mod_vhost.c
mod_vhost_la_LDFLAGS = -module -export-dynamic -avoid-version -no-undefined

pkglib_LTLIBRARIES=$(install_libs)

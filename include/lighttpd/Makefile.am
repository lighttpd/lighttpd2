
headerfiles=$(filter-out $(srcdir)/config.h,$(wildcard $(srcdir)/*.h))

EXTRA_DIST=$(headerfiles) meson.build

headersdir=$(includedir)/lighttpd
headers_DATA=$(headerfiles) config.h

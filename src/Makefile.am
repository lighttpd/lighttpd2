
SUBDIRS=common main modules angel unittests

EXTRA_DIST=lighttpd2.pc.in meson.build

pkgconfigdir = $(libdir)/pkgconfig
pkgconfig_DATA = lighttpd2.pc

$(pkgconfig_DATA): ../config.status


SUBDIRS=common main modules angel unittests

EXTRA_DIST=CMakeLists.txt config.h.cmake lighttpd2.pc.in

pkgconfigdir = $(libdir)/pkgconfig
pkgconfig_DATA = lighttpd2.pc

$(pkgconfig_DATA): ../config.status

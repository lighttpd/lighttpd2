#                                               -*- Autoconf -*-
# Process this file with autoconf to produce a configure script.

AC_PREREQ([2.61])
AC_INIT([lighttpd],[2.0.0],[contact@lighttpd.net])
AC_CONFIG_SRCDIR([src/main/lighttpd_worker.c])
AC_CONFIG_HEADER([include/lighttpd/config.h])
AC_CONFIG_MACRO_DIR([m4])

AM_INIT_AUTOMAKE([-Wall -Wno-portability -Wno-override -Werror foreign dist-bzip2 tar-ustar])

dnl check environment
AC_USE_SYSTEM_EXTENSIONS

# Checks for programs.
AC_PROG_CC
AC_PROG_MAKE_SET
AC_PROG_INSTALL
# AC_PROG_RANLIB
AM_PROG_AR

AC_PATH_PROG([RAGEL], [ragel])
if test "x$RAGEL" = "x"; then
	AC_MSG_ERROR([ragel not found])
fi
AC_SUBST([RAGEL])

dnl libtool
AC_DISABLE_STATIC
AC_ENABLE_SHARED

m4_ifndef([PKG_PROG_PKG_CONFIG], [m4_fatal([pkg-config not installed])])

m4_ifndef([AC_PROG_LIBTOOL], [m4_fatal([libtool not installed])])

AC_PROG_LIBTOOL

# Checks for header files.
AC_HEADER_STDC
AC_HEADER_SYS_WAIT
AC_CHECK_HEADERS([ \
	unistd.h \
	stddef.h \
	sys/mman.h \
	sys/resource.h \
	sys/sendfile.h \
	sys/types.h \
	sys/uio.h \
	sys/un.h \
	execinfo.h \
])

# pkglibdir
AC_ARG_WITH(
  [pkglibdir],
  AS_HELP_STRING([--with-pkglibdir=dir],
                 [specify directory for driver files (default LIBDIR/lighttpd-VERSION]),
  [pkglibdir=$withval],
  [pkglibdir=$libdir/lighttpd-$PACKAGE_VERSION])
AC_SUBST(pkglibdir)

# libexecdir
libexecdir=$libexecdir/lighttpd2
AC_SUBST([libexecdir])

# Checks for libraries.

# glib/gthread (gthread includes glib)
PKG_CHECK_MODULES([GTHREAD], [gthread-2.0 >= 2.16.0], [],[AC_MSG_ERROR("gthread-2.0 >= 2.16.0 not found")])
# gmodule
PKG_CHECK_MODULES([GMODULE], [gmodule-2.0 >= 2.16.0], [],[AC_MSG_ERROR("gmodule-2.0 >= 2.16.0 not found")])

AC_MSG_CHECKING([for libev support])
AC_ARG_WITH([libev],
    [AS_HELP_STRING([--with-libev@<:@=PATH@:>@],[Search for libev in PATH/include and PATH/lib])],
    [WITH_LIBEV=$withval],[WITH_LIBEV=yes])

LIBEV_CFLAGS=""
LIBEV_LIBS=""

PKG_CHECK_MODULES([LIBEV], [libev], [], [
	# no pkg-config for libev, searching manually:

	if test "$WITH_LIBEV" != "yes"; then
		LIBEV_CFLAGS="-I$WITH_LIBEV/include"
		LIBEV_LIBS="-L$WITH_LIBEV/lib -lev"
	else
		AC_CHECK_HEADERS([ev.h],[
			AC_CHECK_LIB([ev], [ev_time], [
					LIBEV_LIBS="-lev"
				],[
					AC_MSG_ERROR([libev not found])
				]
			)],[
				AC_MSG_ERROR([libev not found])
			]
		)
	fi
])

AC_SUBST([LIBEV_CFLAGS])
AC_SUBST([LIBEV_LIBS])

dnl Large File Support

AC_ARG_ENABLE([lfs],
 [AS_HELP_STRING([--disable-lfs],[disable large file support])],
 [case "${enableval}" in
   yes) lfs=true ;;
    no) lfs=false ;;
     *) AC_MSG_ERROR([bad value ${enableval} for --enable-lfs]) ;;
  esac],[lfs=true])

if test x$lfs = xtrue; then
  CFLAGS="${CFLAGS} -D_FILE_OFFSET_BITS=64 -D_LARGEFILE_SOURCE -D_LARGE_FILES"
fi

dnl Check for gnutls
AC_ARG_WITH([gnutls], [AS_HELP_STRING([--with-gnutls],[gnutls library for ssl/tls])],
[WITH_GNUTLS=$withval],[WITH_GNUTLS=no])

if test "$WITH_GNUTLS" != "no"; then
 PKG_CHECK_MODULES([GNUTLS], [gnutls],[],[
  AC_MSG_ERROR([gnutls not found])
 ])

 AC_SUBST([GNUTLS_CFLAGS])
 AC_SUBST([GNUTLS_LIBS])
 USE_GNUTLS=true
fi

AM_CONDITIONAL([USE_GNUTLS], [test "$USE_GNUTLS" = "true"])


dnl Check for libidn, needed to decode SNI names
AC_ARG_WITH([sni], [AS_HELP_STRING([--with-sni],[SNI support for gnutls/openssl, needs libidn])],
[WITH_SNI=$withval],[WITH_SNI=no])

if test "$WITH_SNI" != "no"; then
 PKG_CHECK_MODULES([IDN], [libidn],[],[
  AC_MSG_ERROR([libidn not found])
 ])

 AC_SUBST([IDN_CFLAGS])
 AC_SUBST([IDN_LIBS])
 USE_SNI=true
fi

AM_CONDITIONAL([USE_SNI], [test "$USE_SNI" = "true"])


dnl Check for lua
AC_MSG_CHECKING([for lua])
AC_ARG_WITH([lua], [AS_HELP_STRING([--with-lua],[lua engine (recommended)])],
[WITH_LUA=$withval],[WITH_LUA=no])

AC_MSG_RESULT([$WITH_LUA])
if test "$WITH_LUA" != "no"; then
 # try pkgconfig
 if test "$WITH_LUA" = "yes"; then
    LUAPC=lua
  else
    LUAPC=$WITH_LUA
  fi

 PKG_CHECK_MODULES([LUA], [$LUAPC >= 5.1], [
   AC_DEFINE([HAVE_LUA], [1], [liblua])
   AC_DEFINE([HAVE_LUA_H], [1], [lua.h])
 ],[
 # for debian based systems
 PKG_CHECK_MODULES([LUA], [lua5.1 >= 5.1], [
   AC_DEFINE([HAVE_LUA], [1], [liblua])
   AC_DEFINE([HAVE_LUA_H], [1], [lua.h])
 ],[
 # for freebsd
 PKG_CHECK_MODULES([LUA], [lua-5.1 >= 5.1], [
   AC_DEFINE([HAVE_LUA], [1], [liblua])
   AC_DEFINE([HAVE_LUA_H], [1], [lua.h])
 ],[
   AC_MSG_ERROR([lua not found])
 ])
 ])
 ])

 AC_SUBST([LUA_CFLAGS])
 AC_SUBST([LUA_LIBS])
 USE_LUA=true
fi

AM_CONDITIONAL([USE_LUA], [test "$USE_LUA" = "true"])



# Checks for typedefs, structures, and compiler characteristics.
AC_C_CONST
AC_TYPE_UID_T
AC_TYPE_PID_T
AC_TYPE_SIZE_T
AC_HEADER_TIME

## solaris needs -lsocket -lnsl
AC_SEARCH_LIBS([socket],[socket])
AC_SEARCH_LIBS([inet_addr],[nsl socket])

## freebsd: kvm
AC_SEARCH_LIBS([kvm_open],[kvm])

# Checks for library functions.
AC_CHECK_FUNCS([ \
	chroot \
	getrlimit \
	gmtime_r \
	inet_aton \
	inet_ntop \
	localtime_r \
	madvise \
	mmap \
	posix_fadvise \
	sendfile \
	sendfile64 \
	sendfilev \
	writev \
	accept4 \
])

dnl Check for IPv6 support

AC_ARG_ENABLE([ipv6],
 [AS_HELP_STRING([--disable-ipv6],[disable IPv6 support])],
 [case "${enableval}" in
   yes) ipv6=true ;;
    no) ipv6=false ;;
     *) AC_MSG_ERROR([bad value ${enableval} for --enable-ipv6]) ;;
  esac],[ipv6=true])

if test x$ipv6 = xtrue; then
  AC_CACHE_CHECK([for IPv6 support], [ac_cv_ipv6_support],
  [AC_LINK_IFELSE([AC_LANG_PROGRAM([[ #include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>]], [[struct sockaddr_in6 s; struct in6_addr t=in6addr_any; int i=AF_INET6; s; t.s6_addr[0] = 0; ]])],[ac_cv_ipv6_support=yes],[ac_cv_ipv6_support=no])])

  if test "$ac_cv_ipv6_support" = yes; then
    AC_DEFINE([HAVE_IPV6],[1],[Whether to enable IPv6 support])
  fi
fi

dnl Check for struct sockaddr_storage

AC_CACHE_CHECK([for struct sockaddr_storage support], [ac_cv_sockaddr_storage_support],
  [AC_LINK_IFELSE([AC_LANG_PROGRAM([[ #include <sys/socket.h> ]], [[struct sockaddr_storage s; ]])],[ac_cv_sockaddr_storage_support=yes],[ac_cv_sockaddr_storage_support=no])])

if test "ac_cv_sockaddr_storage_support" = yes; then
  AC_DEFINE([HAVE_SOCKADDR_STORAGE],[1],[Whether we have struct sockaddr_storage])
fi


dnl Check for openssl
AC_MSG_CHECKING([for OpenSSL])
AC_ARG_WITH([openssl],
    AS_HELP_STRING([--with-openssl@<:@=DIR@:>@],[Include openssl support (default no)]),
    [WITH_OPENSSL=$withval],[WITH_OPENSSL=no])

OPENSSL_CFLAGS=""
OPENSSL_LIBS=""

if test "$WITH_OPENSSL" != "no"; then
  use_openssl=yes
  if test "$WITH_OPENSSL" != "yes"; then
    OPENSSL_CFLAGS="-I$WITH_OPENSSL/include"
    OPENSSL_LIBS="-L$WITH_OPENSSL/lib"
  fi
else
  use_openssl=no
fi
AC_MSG_RESULT([$use_openssl])

AC_ARG_WITH([openssl-includes],
  [AS_HELP_STRING([--with-openssl-includes=DIR],[OpenSSL includes])],
  [ use_openssl=yes OPENSSL_CFLAGS="-I$withval" ]
)

AC_ARG_WITH([openssl-libs],
  [AS_HELP_STRING([--with-openssl-libs=DIR],[OpenSSL libraries])],
  [ use_openssl=yes OPENSSL_LIBS="-L$withval" ]
)

AC_ARG_WITH([kerberos5],
  [AS_HELP_STRING([--with-kerberos5],[use Kerberos5 support with OpenSSL])],
  [ use_kerberos=yes ], [use_kerberos=no]
)

if test "x$use_openssl" = "xyes"; then
  if test "x$use_kerberos" != "xyes"; then
    OPENSSL_CFLAGS="$OPENSSL_CFLAGS -DOPENSSL_NO_KRB5"
  fi

  AC_CHECK_HEADERS([openssl/ssl.h])
  OLDLIBS="$LIBS"
  AC_CHECK_LIB([crypto], [BIO_f_base64], [
    AC_CHECK_LIB([ssl], [SSL_new], [
      OPENSSL_LIBS="$OPENSSL_LIBS -lssl -lcrypto"
      have_openssl=yes
      AC_DEFINE([HAVE_OPENSSL], [], [Have openssl])
    ], [], [ -lcrypto "$DL_LIB" ])
  ], [], [])
  LIBS="$OLDLIBS"

  if test "x$have_openssl" != "xyes"; then
    AC_MSG_ERROR([Couldn't find openssl])
  fi

  AC_SUBST([OPENSSL_CFLAGS])
  AC_SUBST([OPENSSL_LIBS])
fi

AM_CONDITIONAL([USE_OPENSSL], [test "x$have_openssl" = "xyes"])

# mod-deflate:

use_mod_deflate=no

# check for zlib
AC_MSG_CHECKING([for zlib support])
AC_ARG_WITH([zlib], [AS_HELP_STRING([--with-zlib],[Enable zlib support for mod_deflate])],
    [WITH_ZLIB=$withval],[WITH_ZLIB=yes])
AC_MSG_RESULT([$WITH_ZLIB])

if test "$WITH_BZIP2" != "no"; then
  AC_CHECK_LIB([z], [deflate], [
    AC_CHECK_HEADERS([zlib.h],[
      Z_LIB=-lz
      use_mod_deflate=yes
      AC_DEFINE([HAVE_ZLIB], [1], [with zlib])
    ])
  ])
fi
AC_SUBST([Z_LIB])


# check for bzip2
AC_MSG_CHECKING([for bzip2 support])
AC_ARG_WITH([bzip2], [AS_HELP_STRING([--with-bzip2],[Enable bzip2 support for mod_deflate])],
    [WITH_BZIP2=$withval],[WITH_BZIP2=yes])
AC_MSG_RESULT([$WITH_BZIP2])

if test "$WITH_BZIP2" != "no"; then
  AC_CHECK_LIB([bz2], [BZ2_bzCompress], [
    AC_CHECK_HEADERS([bzlib.h],[
      BZ_LIB=-lbz2
      use_mod_deflate=yes
      AC_DEFINE([HAVE_BZIP], [1], [with bzip2])
    ])
  ])
fi
AC_SUBST([BZ_LIB])

AM_CONDITIONAL([USE_MOD_DEFLATE], [test "x$use_mod_deflate" = "xyes"])

AC_ARG_ENABLE([profiler],
 [AS_HELP_STRING([--enable-profiler],[enable memory profiler support])],[
   profiler=yes
   AC_DEFINE([WITH_PROFILER], [1], [profiler])
   AC_CHECK_HEADERS([execinfo.h])
 ],[])

AM_CONDITIONAL([WITH_PROFILER], [test "x$profiler" = "xyes"])

dnl search for crypt_r and (fallback) for crypt
save_LIBS=$LIBS
LIBS=
AC_SEARCH_LIBS([crypt_r],[crypt],[
  AC_DEFINE([HAVE_CRYPT_R], [1], [crypt_r])
  AC_CHECK_HEADERS([crypt.h],[
    AC_DEFINE([HAVE_CRYPT_H], [1], [crypt.h])
  ])

  CRYPT_LIB=$LIBS
],[
  AC_SEARCH_LIBS([crypt],[crypt],[
    AC_CHECK_HEADERS([crypt.h],[
      AC_DEFINE([HAVE_CRYPT_H], [1], [crypt.h])
    ])

    CRYPT_LIB=$LIBS
  ])
])
LIBS=$save_LIBS
AC_SUBST([CRYPT_LIB])


# check for extra compiler options (warning options)
if test "${GCC}" = "yes"; then
    CFLAGS="${CFLAGS} -Wall -W -Wshadow -pedantic -std=gnu99"
fi

AC_ARG_ENABLE([extra-warnings],
 [AS_HELP_STRING([--enable-extra-warnings],[enable extra warnings (gcc specific)])],
 [case "${enableval}" in
   yes) extrawarnings=true ;;
    no) extrawarnings=false ;;
     *) AC_MSG_ERROR([bad value ${enableval} for --enable-extra-warnings]) ;;
  esac],[extrawarnings=false])

if test x$extrawarnings = xtrue; then
    CFLAGS="${CFLAGS} -g -O2 -g2 -Wall -Wmissing-declarations -Wdeclaration-after-statement -Wno-pointer-sign -Wcast-align -Wsign-compare -Wnested-externs -Wpointer-arith -Wl,--as-needed -Wformat-security"
fi

AC_CONFIG_FILES([Makefile \
	doc/Makefile \
	include/Makefile \
	include/lighttpd/Makefile \
	src/Makefile \
	src/common/Makefile \
	src/main/Makefile \
	src/angel/Makefile \
	src/modules/Makefile \
	src/unittests/Makefile \
	src/lighttpd2.pc \
	tests/Makefile \
])
AC_OUTPUT

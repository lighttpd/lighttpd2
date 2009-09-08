#                                               -*- Autoconf -*-
# Process this file with autoconf to produce a configure script.

AC_PREREQ(2.61)
AC_INIT([lighttpd], [2.0.0~alpha], [contact@lighttpd.net])
AC_CONFIG_SRCDIR([src/main/lighttpd.c])
AC_CONFIG_HEADER([include/lighttpd/config.h])
AC_CONFIG_MACRO_DIR([m4])

AM_INIT_AUTOMAKE([-Wall -Werror foreign])

dnl check environment
AC_USE_SYSTEM_EXTENSIONS

# Checks for programs.
AC_PROG_CC
AC_PROG_MAKE_SET
AC_PROG_INSTALL
# AC_PROG_RANLIB

dnl libtool
AC_DISABLE_STATIC
AC_ENABLE_SHARED

AC_PROG_LIBTOOL

# Checks for header files.
AC_HEADER_STDC
AC_HEADER_SYS_WAIT
AC_CHECK_HEADERS([ \
	stddef.h \
	sys/mman.h \
	sys/sendfile.h \
	sys/types.h \
	sys/uio.h \
	sys/un.h \
])

# Checks for libraries.

# glib/gthread (gthread includes glib)
PKG_CHECK_MODULES(GTHREAD, gthread-2.0 >= 2.16.0, [],[AC_MSG_ERROR("gthread-2.0 >= 2.16.0 not found")])
# gmodule
PKG_CHECK_MODULES(GMODULE, gmodule-2.0 >= 2.16.0, [],[AC_MSG_ERROR("gmodule-2.0 >= 2.16.0 not found")])

AC_MSG_CHECKING(for libev support)
AC_ARG_WITH(libev,
    AC_HELP_STRING([--with-libev@<:@=PATH@:>@],[Search for libev in PATH/include and PATH/lib]),
    [WITH_LIBEV=$withval],[WITH_LIBEV=/usr])

AC_CHECK_HEADERS([ev.h],[
	AC_CHECK_LIB([ev], [ev_loop], [
		AC_SUBST(LIBEV_CFLAGS, [])
		AC_SUBST(LIBEV_LIBS, [-lev])
		],[
		AC_MSG_ERROR([libev not found])
		]
	)],[
		AC_MSG_ERROR([libev not found])
	]
)

dnl Check for lua
AC_MSG_CHECKING(for lua)
AC_ARG_WITH(lua, AC_HELP_STRING([--with-lua],[lua engine for mod_magnet]),
[WITH_LUA=$withval],[WITH_LUA=no])

AC_MSG_RESULT($WITH_LUA)
if test "$WITH_LUA" != "no"; then
 # try pkgconfig
 if test "$WITH_LUA" = "yes"; then
    LUAPC=lua
  else
    LUAPC=$WITH_LUA
  fi

 PKG_CHECK_MODULES(LUA, $LUAPC >= 5.1, [
   AC_DEFINE([HAVE_LUA], [1], [liblua])
   AC_DEFINE([HAVE_LUA_H], [1], [lua.h])
 ],[
 # for debian based systems
 PKG_CHECK_MODULES(LUA, lua5.1 >= 5.1, [
   AC_DEFINE([HAVE_LUA], [1], [liblua])
   AC_DEFINE([HAVE_LUA_H], [1], [lua.h])
 ],[
 # for freebsd
 PKG_CHECK_MODULES(LUA, lua-5.1 >= 5.1, [
   AC_DEFINE([HAVE_LUA], [1], [liblua])
   AC_DEFINE([HAVE_LUA_H], [1], [lua.h])
 ],[
   AC_MSG_ERROR([lua not found])
 ])
 ])
 ])

 AC_SUBST(LUA_CFLAGS)
 AC_SUBST(LUA_LIBS)
 USE_LUA=true
fi

AM_CONDITIONAL(USE_LUA, test "$USE_LUA" = "true")

# Checks for typedefs, structures, and compiler characteristics.
AC_C_CONST
AC_TYPE_UID_T
AC_TYPE_PID_T
AC_TYPE_SIZE_T
AC_HEADER_TIME

## solaris needs -lsocket -lnsl
AC_SEARCH_LIBS([socket],[socket])
AC_SEARCH_LIBS([inet_addr],[nsl socket])

# Checks for library functions.
AC_CHECK_FUNCS([ \
	chroot \
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
])

dnl Check for IPv6 support

AC_ARG_ENABLE(ipv6,
 AC_HELP_STRING([--disable-ipv6],[disable IPv6 support]),
 [case "${enableval}" in
   yes) ipv6=true ;;
    no) ipv6=false ;;
     *) AC_MSG_ERROR(bad value ${enableval} for --enable-ipv6) ;;
  esac],[ipv6=true])

if test x$ipv6 = xtrue; then
  AC_CACHE_CHECK([for IPv6 support], ac_cv_ipv6_support,
  [AC_TRY_LINK([ #include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>], [struct sockaddr_in6 s; struct in6_addr t=in6addr_any; int i=AF_INET6; s; t.s6_addr[0] = 0; ],
  [ac_cv_ipv6_support=yes], [ac_cv_ipv6_support=no])])

  if test "$ac_cv_ipv6_support" = yes; then
    AC_DEFINE(HAVE_IPV6,1,[Whether to enable IPv6 support])
  fi
fi


dnl Check for openssl
AC_MSG_CHECKING(for OpenSSL)
AC_ARG_WITH(openssl,
    AC_HELP_STRING([--with-openssl@<:@=DIR@:>@],[Include openssl support (default no)]),
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

AC_ARG_WITH(openssl-includes,
  AC_HELP_STRING([--with-openssl-includes=DIR],[OpenSSL includes]),
  [ use_openssl=yes OPENSSL_CFLAGS="-I$withval" ]
)

AC_ARG_WITH(openssl-libs,
  AC_HELP_STRING([--with-openssl-libs=DIR],[OpenSSL libraries]),
  [ use_openssl=yes OPENSSL_LIBS="-L$withval" ]
)

AC_ARG_WITH(kerberos5,
  AC_HELP_STRING([--with-kerberos5],[use Kerberos5 support with OpenSSL]),
  [ use_kerberos=yes ], [use_kerberos=no]
)

if test "x$use_openssl" = "xyes"; then
  if test "x$use_kerberos" != "xyes"; then
    OPENSSL_CFLAGS="$OPENSSL_CFLAGS -DOPENSSL_NO_KRB5"
  fi

  AC_CHECK_HEADERS([openssl/ssl.h])
  OLDLIBS="$LIBS"
  AC_CHECK_LIB(crypto, BIO_f_base64, [
    AC_CHECK_LIB(ssl, SSL_new, [ OPENSSL_LIBS="$OPENSSL_LIBS -lssl -lcrypto"
      AC_DEFINE(HAVE_LIBSSL, [], [Have libssl]) ], [], [ -lcrypto "$DL_LIB" ])
  ], [], [])
  LIBS="$OLDLIBS"

  AC_SUBST(OPENSSL_CFLAGS)
  AC_SUBST(OPENSSL_LIBS)
fi


# check for extra compiler options (warning options)
if test "${GCC}" = "yes"; then
    CFLAGS="${CFLAGS} -Wall -W -Wshadow -pedantic -std=gnu99"
fi

AC_ARG_ENABLE(extra-warnings,
 AC_HELP_STRING([--enable-extra-warnings],[enable extra warnings (gcc specific)]),
 [case "${enableval}" in
   yes) extrawarnings=true ;;
    no) extrawarnings=false ;;
     *) AC_MSG_ERROR(bad value ${enableval} for --enable-extra-warnings) ;;
  esac],[extrawarnings=false])

if test x$extrawarnings = xtrue; then
    CFLAGS="${CFLAGS} -g -O2 -g2 -Wall -Wmissing-declarations -Wdeclaration-after-statement -Wno-pointer-sign -Wcast-align -Winline -Wsign-compare -Wnested-externs -Wpointer-arith -Wl,--as-needed -Wformat-security"
fi

AC_CONFIG_FILES([Makefile src/Makefile src/common/Makefile src/main/Makefile src/angel/Makefile src/modules/Makefile])
AC_OUTPUT

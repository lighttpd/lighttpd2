INCLUDE(CheckCSourceCompiles)
INCLUDE(CheckIncludeFiles)
INCLUDE(CheckFunctionExists)
INCLUDE(CheckVariableExists)
INCLUDE(CheckTypeSize)
INCLUDE(CheckLibraryExists)
INCLUDE(CMakeDetermineCCompiler)
INCLUDE(FindThreads)
INCLUDE(FindPkgConfig)

INCLUDE(LighttpdMacros)

FIND_PACKAGE(Ragel REQUIRED)
FIND_PACKAGE(LibEV REQUIRED)

cmake_policy(VERSION 2.6.4)

ADD_DEFINITIONS(-D_FILE_OFFSET_BITS=64 -D_LARGEFILE_SOURCE -D_LARGE_FILES)

# OPTION(WITH_OPENSSL "with openssl-support [default: on]" ON)
OPTION(WITH_LUA "with lua 5.1 for lua-configfile [default: on]" ON)
OPTION(WITH_OPENSSL "with openssl-support [default: off]")
OPTION(BUILD_STATIC "build a static lighttpd with all modules added")
OPTION(BUILD_EXTRA_WARNINGS "extra warnings")

IF(BUILD_STATIC)
  SET(LIGHTTPD_STATIC 1)
ENDIF(BUILD_STATIC)

SET(CMAKE_REQUIRED_INCLUDES ${CMAKE_SYSTEM_INCLUDE_PATH})

CHECK_INCLUDE_FILES(inttypes.h HAVE_INTTYPES_H)
CHECK_INCLUDE_FILES(stddef.h HAVE_STDDEF_H)
CHECK_INCLUDE_FILES(stdint.h HAVE_STDINT_H)
CHECK_INCLUDE_FILES(sys/mman.h HAVE_SYS_MMAN_H)
CHECK_INCLUDE_FILES(sys/sendfile.h HAVE_SYS_SENDFILE_H)
CHECK_INCLUDE_FILES(sys/types.h HAVE_SYS_TYPES_H)
CHECK_INCLUDE_FILES(sys/uio.h HAVE_SYS_UIO_H)
CHECK_INCLUDE_FILES(sys/un.h HAVE_SYS_UN_H)
CHECK_INCLUDE_FILES(unistd.h HAVE_UNISTD_H)

# will be needed for auth
#CHECK_INCLUDE_FILES(crypt.h HAVE_CRYPT_H)
#IF(HAVE_CRYPT_H)
#  ## check if we need libcrypt for crypt()
#  CHECK_LIBRARY_EXISTS(crypt crypt "" HAVE_LIBCRYPT)
#ENDIF(HAVE_CRYPT_H)
#CHECK_FUNCTION_EXISTS(crypt HAVE_CRYPT)

CHECK_TYPE_SIZE(long SIZEOF_LONG)
CHECK_TYPE_SIZE(off_t SIZEOF_OFF_T)

CHECK_FUNCTION_EXISTS(chroot HAVE_CHROOT)
CHECK_FUNCTION_EXISTS(gmtime_r HAVE_GMTIME_R)
CHECK_FUNCTION_EXISTS(inet_aton HAVE_INET_ATON)
CHECK_FUNCTION_EXISTS(inet_ntop HAVE_INET_NTOP)
CHECK_FUNCTION_EXISTS(localtime_r HAVE_LOCALTIME_R)
CHECK_FUNCTION_EXISTS(madvise HAVE_MADVISE)
CHECK_FUNCTION_EXISTS(mmap HAVE_MMAP)
CHECK_FUNCTION_EXISTS(posix_fadvise HAVE_POSIX_FADVISE)
CHECK_FUNCTION_EXISTS(sendfile HAVE_SENDFILE)
CHECK_FUNCTION_EXISTS(sendfile64 HAVE_SENDFILE64)
CHECK_FUNCTION_EXISTS(sendfilev HAVE_SENDFILEV)
CHECK_FUNCTION_EXISTS(writev HAVE_WRITEV)
CHECK_C_SOURCE_COMPILES("
	#include <sys/types.h>
	#include <sys/socket.h>
	#include <netinet/in.h>
	
	int main() {
		struct sockaddr_in6 s; struct in6_addr t=in6addr_any; int i=AF_INET6; s; t.s6_addr[0] = 0;
		return 0;
	}" HAVE_IPV6)

# glib/gthread
pkg_check_modules(GTHREAD REQUIRED gthread-2.0)
pkg_check_modules(GMODULE REQUIRED gmodule-2.0)

IF(WITH_LUA)
  pkg_search_module(LUA REQUIRED lua lua5.1 lua-5.1)
  SET(HAVE_LIBLUA 1 "Have liblua")
  SET(HAVE_LUA_H  1 "Have liblua header")
ENDIF(WITH_LUA)

IF(WITH_OPENSSL)
  CHECK_INCLUDE_FILES(openssl/ssl.h HAVE_OPENSSL_SSL_H)
  IF(HAVE_OPENSSL_SSL_H)
    CHECK_LIBRARY_EXISTS(crypto BIO_f_base64 "" HAVE_LIBCRYPTO)
    IF(HAVE_LIBCRYPTO)
      SET(OPENSSL_NO_KRB5 1)
      CHECK_LIBRARY_EXISTS(ssl SSL_new "" HAVE_LIBSSL)
    ENDIF(HAVE_LIBCRYPTO)
  ENDIF(HAVE_OPENSSL_SSL_H)
ENDIF(WITH_OPENSSL)

IF(NOT BUILD_STATIC)
  CHECK_INCLUDE_FILES(dlfcn.h HAVE_DLFCN_H)
ENDIF(NOT BUILD_STATIC)

IF(NOT SBINDIR)
    SET(SBINDIR "sbin")
ENDIF(NOT SBINDIR)

IF(NOT LIGHTTPD_MODULES_DIR)
    SET(LIGHTTPD_MODULES_DIR "lib${LIB_SUFFIX}/lighttpd")
ENDIF(NOT LIGHTTPD_MODULES_DIR)

IF(NOT WIN32)
ADD_DEFINITIONS(
  -DLIBRARY_DIR="\\"${CMAKE_INSTALL_PREFIX}/${LIGHTTPD_MODULES_DIR}\\""
)
ELSE(NOT WIN32)
## We use relative path in windows
ADD_DEFINITIONS(
  -DLIBRARY_DIR="\\"lib\\""
)
ENDIF(NOT WIN32)

## Write out config.h
CONFIGURE_FILE(${CMAKE_CURRENT_SOURCE_DIR}/config.h.cmake ${CMAKE_BINARY_DIR}/include/lighttpd/config.h)

ADD_DEFINITIONS(-DHAVE_CONFIG_H)

INCLUDE_DIRECTORIES(${CMAKE_CURRENT_BINARY_DIR} ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/include  ${CMAKE_BINARY_DIR}/include)

SET(COMMON_SRC
	angel_connection.c
	angel_data.c
	encoding.c
	idlist.c
	ip_parsers.c
	module.c
	radix.c
	sys-files.c
	sys-socket.c
	utils.c
	waitqueue.c
)

ADD_PREFIX(COMMON_SRC common/)

SET(LIGHTTPD_SHARED_SRC
	angel.c
	angel_fake.c
	actions.c
	chunk.c
	chunk_parser.c
	collect.c
	condition.c
	config_parser.c
	connection.c
	environment.c
	etag.c
	filter_chunked.c
	http_headers.c
	http_request_parser.c
	http_response_parser.c
	lighttpd-glue.c
	log.c
	network.c
	network_write.c network_writev.c
	network_sendfile.c
	options.c
	plugin.c
	profiler.c
	request.c
	response.c
	server.c
	stat_cache.c
	throttle.c
	url_parser.c
	value.c
	virtualrequest.c
	worker.c

	plugin_core.c
)

IF(WITH_LUA)
SET(LIGHTTPD_SHARED_SRC ${LIGHTTPD_SHARED_SRC}
	actions_lua.c
	condition_lua.c
	config_lua.c
	value_lua.c

	chunk_lua.c
	core_lua.c
	connection_lua.c
	environment_lua.c
	http_headers_lua.c
	physical_lua.c
	request_lua.c
	response_lua.c
	virtualrequest_lua.c
)
ENDIF(WITH_LUA)

ADD_PREFIX(LIGHTTPD_SHARED_SRC main/)

SET(ANGEL_SHARED_SRC
	angel_config_parser.c
	angel_log.c
	angel_plugin.c
	angel_plugin_core.c
	angel_proc.c
	angel_server.c
	angel_value.c
)
ADD_PREFIX(ANGEL_SHARED_SRC angel/)

## Build parsers by using ragel...
RAGEL_PARSER(main/config_parser.rl -T0)
RAGEL_PARSER(main/http_request_parser.rl)
RAGEL_PARSER(main/http_response_parser.rl)
RAGEL_PARSER(common/ip_parsers.rl)
RAGEL_PARSER(main/url_parser.rl)
RAGEL_PARSER(angel/angel_config_parser.rl)

SET(L_INSTALL_TARGETS)

ADD_LIBRARY(lighttpd-common SHARED
	${COMMON_SRC}
)

ADD_EXECUTABLE(lighttpd
	main/lighttpd.c
	${LIGHTTPD_SHARED_SRC}
)
TARGET_LINK_LIBRARIES(lighttpd lighttpd-common)
SET(L_INSTALL_TARGETS ${L_INSTALL_TARGETS} lighttpd)

ADD_EXECUTABLE(lighttpd-angel
	angel/angel_main.c
	${ANGEL_SHARED_SRC}
)
TARGET_LINK_LIBRARIES(lighttpd-angel lighttpd-common)
SET(L_INSTALL_TARGETS ${L_INSTALL_TARGETS} lighttpd-angel)

IF(BUILD_EXTRA_WARNINGS)
	SET(WARN_CFLAGS "-g -O2 -g2 -Wall -Wmissing-declarations -Wdeclaration-after-statement -Wcast-align -Wsign-compare -Wnested-externs -Wpointer-arith")
	SET(WARN_LDFLAGS "-g -O2 -g2 -Wall -Wl,--as-needed")
	# -Werror -Wbad-function-cast -Wmissing-prototypes
ELSE(BUILD_EXTRA_WARNINGS)
	SET(WARN_CFLAGS "")
	SET(WARN_LDFLAGS "")
ENDIF(BUILD_EXTRA_WARNINGS)

SET(COMMON_LDFLAGS "${LUA_LDFLAGS} ${LIBEV_LDFLAGS} ${GTHREAD_LDFLAGS} ${GMODULE_LDFLAGS} ${WARN_LDFLAGS}")
SET(COMMON_CFLAGS "${LUA_CFLAGS} ${LIBEV_CFLAGS} ${GTHREAD_CFLAGS} ${GMODULE_CFLAGS} ${WARN_CFLAGS}")

ADD_AND_INSTALL_LIBRARY(mod_access "modules/mod_access.c")
ADD_AND_INSTALL_LIBRARY(mod_accesslog "modules/mod_accesslog.c")
ADD_AND_INSTALL_LIBRARY(mod_auth "modules/mod_auth.c")
ADD_AND_INSTALL_LIBRARY(mod_balancer "modules/mod_balancer.c")
ADD_AND_INSTALL_LIBRARY(mod_cache_disk_etag "modules/mod_cache_disk_etag.c")
ADD_AND_INSTALL_LIBRARY(mod_debug "modules/mod_debug.c")
ADD_AND_INSTALL_LIBRARY(mod_dirlist "modules/mod_dirlist.c")
ADD_AND_INSTALL_LIBRARY(mod_expire "modules/mod_expire.c")
ADD_AND_INSTALL_LIBRARY(mod_fastcgi "modules/mod_fastcgi.c")
ADD_AND_INSTALL_LIBRARY(mod_fortune "modules/mod_fortune.c")
ADD_AND_INSTALL_LIBRARY(mod_redirect "modules/mod_redirect.c")
ADD_AND_INSTALL_LIBRARY(mod_rewrite "modules/mod_rewrite.c")
ADD_AND_INSTALL_LIBRARY(mod_status "modules/mod_status.c")
ADD_AND_INSTALL_LIBRARY(mod_vhost "modules/mod_vhost.c")

IF(HAVE_LIBSSL AND HAVE_LIBCRYPTO)
  ADD_AND_INSTALL_LIBRARY(mod_openssl "modules/mod_openssl.c")
  TARGET_LINK_LIBRARIES(mod_openssl ssl)
  TARGET_LINK_LIBRARIES(mod_openssl crypto)
ENDIF(HAVE_LIBSSL AND HAVE_LIBCRYPTO)


ADD_TARGET_PROPERTIES(lighttpd-common LINK_FLAGS ${COMMON_LDFLAGS})
ADD_TARGET_PROPERTIES(lighttpd-common COMPILE_FLAGS ${COMMON_CFLAGS})

ADD_TARGET_PROPERTIES(lighttpd LINK_FLAGS ${COMMON_LDFLAGS})
ADD_TARGET_PROPERTIES(lighttpd COMPILE_FLAGS ${COMMON_CFLAGS})

ADD_TARGET_PROPERTIES(lighttpd-angel LINK_FLAGS ${COMMON_LDFLAGS})
ADD_TARGET_PROPERTIES(lighttpd-angel COMPILE_FLAGS ${COMMON_CFLAGS})

IF(CMAKE_COMPILER_IS_GNUCC)
  SET(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -std=gnu99 -Wall -g -Wshadow -W -pedantic")
  SET(CMAKE_C_FLAGS_RELEASE        "${CMAKE_C_FLAGS_RELEASE}     -O2")
  SET(CMAKE_C_FLAGS_DEBUG          "${CMAKE_C_FLAGS_DEBUG}       -O0")
  SET(CMAKE_C_FLAGS_RELWITHDEBINFO "${CMAKE_C_FLAGS_WITHDEBINFO} -O2")
  ADD_DEFINITIONS(-D_GNU_SOURCE)
ENDIF(CMAKE_COMPILER_IS_GNUCC)

SET_TARGET_PROPERTIES(lighttpd PROPERTIES CMAKE_INSTALL_PREFIX "${CMAKE_INSTALL_PREFIX}")

IF(WIN32)
  SET(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -DNVALGRIND")
  ADD_TARGET_PROPERTIES(lighttpd COMPILE_FLAGS "-DLI_DECLARE_EXPORTS")
  TARGET_LINK_LIBRARIES(lighttpd ws2_32)

  IF(MINGW)
    TARGET_LINK_LIBRARIES(lighttpd msvcr70)
    ADD_TARGET_PROPERTIES(lighttpd LINK_FLAGS "-Wl,-subsystem,console")
  ENDIF(MINGW)
ENDIF(WIN32)

IF(NOT WIN32)
INSTALL(TARGETS ${L_INSTALL_TARGETS}
	RUNTIME DESTINATION ${SBINDIR}
	LIBRARY DESTINATION ${LIGHTTPD_MODULES_DIR}
	ARCHIVE DESTINATION ${LIGHTTPD_MODULES_DIR}/static)
ELSE(NOT WIN32)
## HACK to make win32 to install our libraries in desired directory..
INSTALL(TARGETS lighttpd
	RUNTIME DESTINATION ${SBINDIR}
	ARCHIVE DESTINATION lib/static)
LIST(REMOVE_ITEM L_INSTALL_TARGETS lighttpd)
INSTALL(TARGETS ${L_INSTALL_TARGETS}
	RUNTIME DESTINATION ${SBINDIR}/lib
	LIBRARY DESTINATION lib
	ARCHIVE DESTINATION lib/static)
ENDIF(NOT WIN32)

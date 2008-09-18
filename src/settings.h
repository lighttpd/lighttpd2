#ifndef _LIGHTTPD_SETTINGS_H_
#define _LIGHTTPD_SETTINGS_H_

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#if defined HAVE_LIBSSL && defined HAVE_OPENSSL_SSL_H
# define USE_OPENSSL
# include <openssl/ssl.h>
#endif

#include <ev.h>
#include <glib.h>
#include <gmodule.h>

#define L_GOFFSET_FORMAT G_GINT64_FORMAT
#define L_GOFFSET_MODIFIER G_GINT64_MODIFIER


#include <assert.h>

#ifdef HAVE_SYS_TYPES_H
# include <sys/types.h>
#endif

#ifdef HAVE_STDINT_H
# include <stdint.h>
#endif

#ifdef HAVE_STDDEF_H
# include <stddef.h>
#endif

#ifdef HAVE_INTTYPES_H
# include <inttypes.h>
#endif

#ifdef HAVE_UNISTD_H
# include <unistd.h>
#endif

#ifdef HAVE_PCRE_H
#include <pcre.h>
#endif

#include <errno.h>
#include <string.h>

/**
 * if glib supports threads we will use it for async file-io
 */
#ifdef G_THREADS_ENABLED
# ifndef USE_GTHREAD
#  define USE_GTHREAD
# endif
#endif

/* on linux 2.4.x you get either sendfile or LFS */
#if defined HAVE_SYS_SENDFILE_H && defined HAVE_SENDFILE && (!defined _LARGEFILE_SOURCE || defined HAVE_SENDFILE64) && defined HAVE_WRITEV && defined(__linux__) && !defined HAVE_SENDFILE_BROKEN
# define USE_LINUX_SENDFILE
# include <sys/sendfile.h>
# include <sys/uio.h>
#endif

/* all the Async IO backends need GTHREAD support */
#if defined(USE_GTHREAD)
# if defined(USE_LINUX_SENDFILE)
#  if 0 && defined(HAVE_LIBAIO_H)
     /** disabled for now as not all FSs are async-io capable */
#    define USE_LINUX_AIO_SENDFILE
#  endif
#  define USE_GTHREAD_SENDFILE
# endif
# if defined(HAVE_AIO_H) && (!defined(__FreeBSD__))
/* FreeBSD has no SIGEV_THREAD for us */
#  define USE_POSIX_AIO
#  include <sys/types.h> /* macosx wants it */
#  include <aio.h>
# endif
# ifdef HAVE_MMAP
#  define USE_GTHREAD_AIO
# endif
#endif

#if defined HAVE_SYS_UIO_H && defined HAVE_SENDFILE && defined HAVE_WRITEV && (defined(__FreeBSD__) || defined(__DragonFly__))
# define USE_FREEBSD_SENDFILE
# include <sys/uio.h>
#endif

#if defined HAVE_SYS_SENDFILE_H && defined HAVE_SENDFILEV && defined HAVE_WRITEV && defined(__sun)
# define USE_SOLARIS_SENDFILEV
# include <sys/sendfile.h>
# include <sys/uio.h>
#endif

#if defined HAVE_SYS_UIO_H && defined HAVE_WRITEV
# define USE_WRITEV
# include <sys/uio.h>
#endif

#if defined HAVE_SYS_MMAN_H && defined HAVE_MMAP
# define USE_MMAP
# include <sys/mman.h>
/* NetBSD 1.3.x needs it */
# ifndef MAP_FAILED
#  define MAP_FAILED -1
# endif

#if defined(MAP_ANON)
#define HAVE_MEM_MMAP_ANON
#else
/* let's try /dev/zero */
#define HAVE_MEM_MMAP_ZERO
#endif

#endif

#if defined HAVE_SYS_UIO_H && defined HAVE_WRITEV && defined HAVE_SEND_FILE && defined(__aix)
# define USE_AIX_SENDFILE
#endif


/**
* unix can use read/write or recv/send on sockets
* win32 only recv/send
*/
#ifdef _WIN32

# define WIN32_LEAN_AND_MEAN
# define NOGDI
# define USE_WIN32_SEND
/* wait for async-io support
# define USE_WIN32_TRANSMITFILE
*/
#else
# define USE_WRITE
#endif


typedef enum {
	HANDLER_GO_ON,
	HANDLER_FINISHED,
	HANDLER_COMEBACK,
	HANDLER_WAIT_FOR_EVENT,
	HANDLER_ERROR,
	HANDLER_WAIT_FOR_FD
} handler_t;

/* Shared library support */
#ifdef _WIN32
  #define LI_IMPORT __declspec(dllimport)
  #define LI_EXPORT __declspec(dllexport)
  #define LI_DLLLOCAL
  #define LI_DLLPUBLIC
#else
  #define LI_IMPORT
  #ifdef GCC_HASCLASSVISIBILITY
    #define LI_EXPORT __attribute__ ((visibility("default")))
    #define LI_DLLLOCAL __attribute__ ((visibility("hidden")))
    #define LI_DLLPUBLIC __attribute__ ((visibility("default")))
  #else
    #define LI_EXPORT
    #define LI_DLLLOCAL
    #define LI_DLLPUBLIC
  #endif
#endif

#ifdef LI_DECLARE_EXPORTS
#define LI_API LI_EXPORT
#else
#define LI_API LI_IMPORT
#endif

/* Throwable classes must always be visible on GCC in all binaries */
#ifdef _WIN32
  #define LI_EXCEPTIONAPI(api) api
#elif defined(GCC_HASCLASSVISIBILITY)
  #define LI_EXCEPTIONAPI(api) LI_EXPORT
#else
  #define LI_EXCEPTIONAPI(api)
#endif

#ifdef UNUSED_PARAM
#elif defined(__GNUC__)
# define UNUSED_PARAM(x) UNUSED_ ## x __attribute__((unused))
#elif defined(__LCLINT__)
# define UNUSED_PARAM(x) /*@unused@*/ x
#else
# define UNUSED_PARAM(x) x
#endif

#define UNUSED(x) ( (void)(x) )

#if __GNUC__
#define INLINE static inline
// # define INLINE extern inline
#else
# define INLINE static
#endif

#include "sys-files.h"
#include "sys-mmap.h"
#include "sys-process.h"
#include "sys-socket.h"
#include "sys-strings.h"

#endif

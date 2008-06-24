#ifndef _SYS_FILES_H_
#define _SYS_FILES_H_

#define DIR_SEPERATOR_UNIX      '/'
#define DIR_SEPERATOR_UNIX_STR  "/"
#define DIR_SEPERATOR_WIN       '\\'
#define DIR_SEPERATOR_WIN_STR   "\\"

#include "settings.h"

#ifdef _WIN32
#include <windows.h>
#include <io.h>     /* open */
#include <direct.h> /* chdir */

#define DIR_SEPERATOR     DIR_SEPERATOR_WIN
#define DIR_SEPERATOR_STR DIR_SEPERATOR_WIN_STR

#define __S_ISTYPE(mode, mask)  (((mode) & _S_IFMT) == (mask))

#undef S_ISDIR
#undef S_ISCHR
#undef S_ISBLK
#undef S_ISREG
#define S_ISDIR(mode)    __S_ISTYPE((mode), _S_IFDIR)
#define S_ISCHR(mode)    __S_ISTYPE((mode), _S_IFCHR)
#define S_ISBLK(mode)    __S_ISTYPE((mode), _S_IFBLK)
#define S_ISREG(mode)    __S_ISTYPE((mode), _S_IFREG)
/* we don't support symlinks */
#define S_ISLNK(mode)    0

#define lstat stat
#define mkstemp(x) open(mktemp(x), O_RDWR)
#define mkdir(x, y) mkdir(x)

/* retrieve the most recent network, or general libc error */
#define light_sock_errno() (WSAGetLastError())

struct dirent {
    const char *d_name;
};

typedef struct {
    HANDLE h;
    WIN32_FIND_DATA finddata;
    struct dirent dent;
} DIR;

LI_EXPORT DIR * opendir(const char *dn);
LI_EXPORT struct dirent * readdir(DIR *d);
LI_EXPORT void closedir(DIR *d);

LI_EXPORT GString * filename_unix2local(GString *b);
LI_EXPORT GString * pathname_unix2local(GString *b);

#else /* _WIN32 */
#include <unistd.h>
#include <dirent.h>

#define DIR_SEPERATOR     DIR_SEPERATOR_UNIX
#define DIR_SEPERATOR_STR DIR_SEPERATOR_UNIX_STR

#define light_sock_errno() (errno)

#define filename_unix2local(x) /* (x) */
#define pathname_unix2local(x) /* (x) */
#endif /* _WIN32 */

#define PATHNAME_APPEND_SLASH(x) \
	if (x->len > 1 && x->ptr[x->len - 1] != DIR_SEPERATOR) { \
		g_string_append_c(DIR_SEPEARATOR); \
	}

#ifndef O_LARGEFILE
# define O_LARGEFILE 0
#endif

#ifndef O_NOATIME
# define O_NOATIME 0
#endif

#endif



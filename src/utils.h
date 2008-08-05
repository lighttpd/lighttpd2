#ifndef _LIGHTTPD_UTILS_H_
#define _LIGHTTPD_UTILS_H_

#include "settings.h"

LI_API void fatal(const gchar* msg);

/* set O_NONBLOCK and FD_CLOEXEC */
LI_API void fd_init(int fd);

#endif

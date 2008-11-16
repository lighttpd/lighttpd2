#ifndef _SYS_PROCESS_H_
#define _SYS_PROCESS_H_

#ifdef _WIN32
#include <process.h>
#define pid_t int
/* win32 has no fork() */
#define kill(x, y) do { } while (0)
#define getpid() 0

#else
#include <sys/wait.h>
#include <unistd.h>
#endif

#endif


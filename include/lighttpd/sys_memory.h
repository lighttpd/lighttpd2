#ifndef _LIGHTTPD_SYS_MEMORY_H_
#define _LIGHTTPD_SYS_MEMORY_H_

/* returns the currently used memory (RSS, resident set size) in bytes or 0 on failure */
LI_API gsize li_memory_usage(void);

#endif

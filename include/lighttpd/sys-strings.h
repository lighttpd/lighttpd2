#ifndef _SYS_STRINGS_H_
#define _SYS_STRINGS_H_

#ifdef _WIN32
#define strcasecmp stricmp
#define strncasecmp strnicmp
#include <stdlib.h>
#define str_to_off_t(p, e, b) _strtoi64(p, e, b)
#define STR_OFF_T_MAX LLONG_MAX
#define STR_OFF_T_MIN LLONG_MIN 
#define strtoull _strtoui64
#ifdef __MINGW32__
/* missing prototype */
unsigned __int64 _strtoui64(
		const char *nptr,
		char **endptr,
		int base 
		);
__int64 _strtoi64(
		const char *nptr,
		char **endptr,
		int base 
		);
#endif
#else /** we are a unix */
#include <stdlib.h>
/**
 * we use strtoll() for parsing the ranges into a off_t
 *
 * if off_t is 32bit, we can use strtol() instead
 */
 #if SIZEOF_OFF_T == SIZEOF_LONG
  #define str_to_off_t(p, e, b) strtol(p, e, b)
  #define STR_OFF_T_MAX LONG_MAX
  #define STR_OFF_T_MIN LONG_MIN 
 #elif defined(HAVE_STRTOLL)
  #define str_to_off_t(p, e, b) strtoll(p, e, b)
  #define STR_OFF_T_MAX LLONG_MAX
  #define STR_OFF_T_MIN LLONG_MIN 
 #else
  #error off_t is more than 4 bytes but we can not parse it with strtol() (run autogen.sh again if you build from svn)
 #endif
#endif

#endif


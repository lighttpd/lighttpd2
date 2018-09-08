#ifndef _LIGHTTPD_PLUGIN_CORE_H_
#define _LIGHTTPD_PLUGIN_CORE_H_

#include <lighttpd/base.h>

typedef enum { LI_ETAG_USE_INODE = 1, LI_ETAG_USE_MTIME = 2, LI_ETAG_USE_SIZE = 4 } liETagFlags;

enum liCoreOptions {
	LI_CORE_OPTION_DEBUG_REQUEST_HANDLING = 0,

	LI_CORE_OPTION_STATIC_RANGE_REQUESTS,

	LI_CORE_OPTION_MAX_KEEP_ALIVE_IDLE,
	LI_CORE_OPTION_MAX_KEEP_ALIVE_REQUESTS,

	LI_CORE_OPTION_ETAG_FLAGS,

	LI_CORE_OPTION_ASYNC_STAT,

	LI_CORE_OPTION_BUFFER_ON_DISK_REQUEST_BODY,

	LI_CORE_OPTION_STRICT_POST_CONTENT_LENGTH,
};

enum liCoreOptionPtrs {
	LI_CORE_OPTION_STATIC_FILE_EXCLUDE_EXTENSIONS = 0,

	LI_CORE_OPTION_SERVER_NAME,
	LI_CORE_OPTION_SERVER_TAG,

	LI_CORE_OPTION_MIME_TYPES,
};

/* the core plugin always has base index 0, as it is the first plugin loaded */
#define CORE_OPTION(idx) _CORE_OPTION(vr, idx)
#define _CORE_OPTION(vr, idx) _OPTION_ABS(vr, idx)
#define CORE_OPTIONPTR(idx) _CORE_OPTIONPTR(vr, idx)
#define _CORE_OPTIONPTR(vr, idx) _OPTIONPTR_ABS(vr, idx)

LI_API void li_plugin_core_init(liServer *srv, liPlugin *p, gpointer userdata);

#endif

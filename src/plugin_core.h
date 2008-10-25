#ifndef _LIGHTTPD_PLUGIN_CORE_H_
#define _LIGHTTPD_PLUGIN_CORE_H_

enum core_options_t {
	CORE_OPTION_DEBUG_REQUEST_HANDLING = 0,

	CORE_OPTION_LOG_TS_FORMAT,
	CORE_OPTION_LOG,

	CORE_OPTION_STATIC_FILE_EXCLUDE,

	CORE_OPTION_SERVER_TAG,
	CORE_OPTION_MAX_KEEP_ALIVE_IDLE,

	CORE_OPTION_MIME_TYPES,

	CORE_OPTION_DOCROOT
};

/* the core plugin always has base index 0, as it is the first plugin loaded */
#define CORE_OPTION(idx) _CORE_OPTION(vr, idx)
#define _CORE_OPTION(vr, idx) _OPTION_ABS(vr, idx)

#endif

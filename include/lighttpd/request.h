#ifndef _LIGHTTPD_REQUEST_H_
#define _LIGHTTPD_REQUEST_H_

#ifndef _LIGHTTPD_BASE_H_
#error Please include <lighttpd/base.h> instead of this file
#endif

struct request_uri {
	GString *raw;

	GString *scheme;
	GString *authority;
	GString *path;
	GString *query;

	GString *host; /* without userinfo and port */
};

struct physical {
	GString *path;
	GString *basedir;

	GString *doc_root;
	GString *rel_path;

	GString *pathinfo;

	gint64 size;
};

struct request {
	http_method_t http_method;
	GString *http_method_str;
	http_version_t http_version;

	request_uri uri;

	http_headers *headers;
	/* Parsed headers: */
	goffset content_length;
};

LI_API void request_init(request *req);
LI_API void request_reset(request *req);
LI_API void request_clear(request *req);

LI_API gboolean request_validate_header(connection *con);

LI_API void physical_init(physical *phys);
LI_API void physical_reset(physical *phys);
LI_API void physical_clear(physical *phys);

#endif

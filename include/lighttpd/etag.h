#ifndef _LIGHTTPD_ETAG_H_
#define _LIGHTTPD_ETAG_H_

#ifndef _LIGHTTPD_BASE_H_
#error Please include <lighttpd/base.h> instead of this file
#endif

typedef enum { TRI_FALSE, TRI_MAYBE, TRI_TRUE } tristate_t;

LI_API tristate_t http_response_handle_cachable_etag(vrequest *vr, GString *etag);
LI_API tristate_t http_response_handle_cachable_modified(vrequest *vr, GString *last_modified);

/* mut maybe the same as etag */
LI_API void etag_mutate(GString *mut, GString *etag);
LI_API void etag_set_header(vrequest *vr, struct stat *st, gboolean *cachable);

#endif

#ifndef _LIGHTTPD_ETAG_H_
#define _LIGHTTPD_ETAG_H_

#ifndef _LIGHTTPD_BASE_H_
#error Please include <lighttpd/base.h> instead of this file
#endif

LI_API liTristate li_http_response_handle_cachable_etag(liVRequest *vr, GString *etag);
LI_API liTristate li_http_response_handle_cachable_modified(liVRequest *vr, GString *last_modified);
LI_API gboolean li_http_response_handle_cachable(liVRequest *vr);

/* mut maybe the same as etag */
LI_API void li_etag_mutate(GString *mut, GString *etag);
LI_API void li_etag_set_header(liVRequest *vr, struct stat *st, gboolean *cachable);

#endif

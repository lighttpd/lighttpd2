#ifndef _LIGHTTPD_LIGHTTPD_GLUE_H_
#define _LIGHTTPD_LIGHTTPD_GLUE_H_

#ifndef _LIGHTTPD_BASE_H_
#error Please include <lighttpd/base.h> instead of this file
#endif

/* returns the description for a given http status code and sets the len to the length of the returned string */
LI_API gchar *http_status_string(guint status_code, guint *len);
/* returns the http method as a string and sets len to the length of the returned string */
LI_API gchar *http_method_string(liHttpMethod method, guint *len);
/* returns the http version as a string and sets len to the length of the returned string */
LI_API gchar *http_version_string(liHttpVersion method, guint *len);
/* converts a given 3 digit http status code to a gchar[3] string. e.g. 403 to {'4','0','3'} */
LI_API void http_status_to_str(gint status_code, gchar status_str[]);

/* looks up the mimetype for a filename by comparing suffixes. first match is returned. do not free the result */
LI_API GString *mimetype_get(liVRequest *vr, GString *filename);

#endif

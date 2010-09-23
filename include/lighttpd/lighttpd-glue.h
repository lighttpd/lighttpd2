#ifndef _LIGHTTPD_LIGHTTPD_GLUE_H_
#define _LIGHTTPD_LIGHTTPD_GLUE_H_

/* returns the description for a given http status code and sets the len to the length of the returned string */
LI_API gchar *li_http_status_string(guint status_code, guint *len);
/* returns the liHttpMethod enum entry matching the given string */
LI_API liHttpMethod li_http_method_from_string(const gchar *method_str, gssize len);
/* returns the http method as a string and sets len to the length of the returned string */
LI_API gchar *li_http_method_string(liHttpMethod method, guint *len);
/* returns the http version as a string and sets len to the length of the returned string */
LI_API gchar *li_http_version_string(liHttpVersion method, guint *len);
/* converts a given 3 digit http status code to a gchar[3] string. e.g. 403 to {'4','0','3'} */
LI_API void li_http_status_to_str(gint status_code, gchar status_str[]);

#endif

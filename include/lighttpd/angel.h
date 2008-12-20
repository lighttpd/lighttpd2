#ifndef _LIGHTTPD_ANGEL_H_
#define _LIGHTTPD_ANGEL_H_

/* interface to the angel; implementation needs to work without angel too */

/* listen to a socket */
LI_API int angel_listen(server *srv, GString *str);

/* send log messages while startup to angel, frees the string */
LI_API gboolean angel_log(server *srv, GString *str);


/* angle_fake definitions, only for internal use */
int angel_fake_listen(server *srv, GString *str);
gboolean angel_fake_log(server *srv, GString *str);

#endif

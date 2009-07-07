#ifndef _LIGHTTPD_ANGEL_H_
#define _LIGHTTPD_ANGEL_H_

/* interface to the angel; implementation needs to work without angel too */
LI_API void angel_setup(server *srv);

/* listen to a socket */
LI_API void angel_listen(server *srv, GString *str);

/* send log messages during startup to angel, frees the string */
LI_API void angel_log(server *srv, GString *str);


/* angle_fake definitions, only for internal use */
int angel_fake_listen(server *srv, GString *str);
gboolean angel_fake_log(server *srv, GString *str);

#endif

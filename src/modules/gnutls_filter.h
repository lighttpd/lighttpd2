#ifndef _LIGHTTPD_GNUTLS_FILTER_H_
#define _LIGHTTPD_GNUTLS_FILTER_H_

#include <lighttpd/base.h>

#include <gnutls/gnutls.h>

typedef struct liGnuTLSFilter liGnuTLSFilter;

typedef void (*liGnuTLSFilterHandshakeCB)(liGnuTLSFilter *f, gpointer data, liStream *plain_source, liStream *plain_drain);
typedef void (*liGnuTLSFilterClosedCB)(liGnuTLSFilter *f, gpointer data);
typedef int (*liGnuTLSFilterPostClientHelloCB)(liGnuTLSFilter *f, gpointer data);

typedef struct liGnuTLSFilterCallbacks liGnuTLSFilterCallbacks;
struct liGnuTLSFilterCallbacks {
	liGnuTLSFilterHandshakeCB handshake_cb; /* called after initial handshake is done */
	liGnuTLSFilterClosedCB closed_cb;
	liGnuTLSFilterPostClientHelloCB post_client_hello_cb;
};

LI_API liGnuTLSFilter* li_gnutls_filter_new(
	liServer *srv, liWorker *wrk,
	const liGnuTLSFilterCallbacks *callbacks, gpointer data,
	gnutls_session_t session, liStream *crypt_source, liStream *crypt_drain);

/* doesn't call closed_cb; but you can call this from closed_cb */
LI_API void li_gnutls_filter_free(liGnuTLSFilter *f);

#endif

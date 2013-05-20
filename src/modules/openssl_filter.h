#ifndef _LIGHTTPD_OPENSSL_FILTER_H_
#define _LIGHTTPD_OPENSSL_FILTER_H_

#include <lighttpd/base.h>

#include <openssl/ssl.h>

typedef struct liOpenSSLFilter liOpenSSLFilter;

typedef void (*liOpenSSLFilterHandshakeCB)(liOpenSSLFilter *f, gpointer data, liStream *plain_source, liStream *plain_drain);
typedef void (*liOpenSSLFilterClosedCB)(liOpenSSLFilter *f, gpointer data);

typedef struct liOpenSSLFilterCallbacks liOpenSSLFilterCallbacks;
struct liOpenSSLFilterCallbacks {
	liOpenSSLFilterHandshakeCB handshake_cb; /* called after initial handshake is done */
	liOpenSSLFilterClosedCB closed_cb;
};

LI_API liOpenSSLFilter* li_openssl_filter_new(
	liServer *srv, liWorker *wrk,
	const liOpenSSLFilterCallbacks *callbacks, gpointer data,
	SSL_CTX *ssl_ctx, liStream *crypt_source, liStream *crypt_drain);

/* doesn't call closed_cb; but you can call this from closed_cb */
LI_API void li_openssl_filter_free(liOpenSSLFilter *f);

LI_API SSL* li_openssl_filter_ssl(liOpenSSLFilter *f);

#endif

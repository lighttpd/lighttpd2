#ifndef _LIGHTTPD_GNUTLS_OCSP_H_
#define _LIGHTTPD_GNUTLS_OCSP_H_

#include <lighttpd/base.h>

#include <gnutls/gnutls.h>

typedef struct liGnuTLSOCSP liGnuTLSOCSP;

LI_API liGnuTLSOCSP* li_gnutls_ocsp_new(void);

/* doesn't call closed_cb; but you can call this from closed_cb */
LI_API void li_gnutls_ocsp_free(liGnuTLSOCSP *ocsp);

LI_API void li_gnutls_ocsp_use(liGnuTLSOCSP *ocsp, gnutls_certificate_credentials_t creds);

/* load DER or PEM ("OCSP RESPONSE") encoded ocsp response */
LI_API gboolean li_gnutls_ocsp_add(liServer *srv, liGnuTLSOCSP *ocsp, const char* filename);

/* search in PEM file for a OCSP RESPONSE block and add it if there is one;
 * returns only FALSE if a block was found which COULDN'T be loaded
 */
LI_API gboolean li_gnutls_ocsp_search(liServer *srv, liGnuTLSOCSP *ocsp, const char* filename);

/* search in PEM datum for a OCSP RESPONSE block and add it if there is one;
 * returns only FALSE if a block was found which COULDN'T be loaded
 */
LI_API gboolean li_gnutls_ocsp_search_datum(liServer *srv, liGnuTLSOCSP *ocsp, gnutls_datum_t const* file);

#endif


#include "gnutls_ocsp.h"

#include <gnutls/ocsp.h>
#include <gnutls/crypto.h>

typedef struct ocsp_response_cert_entry {
	gnutls_digest_algorithm_t digest;
	gnutls_datum_t issuer_name_hash;
	gnutls_datum_t serial;
} ocsp_response_cert_entry;

typedef struct ocsp_response {
	gnutls_ocsp_resp_t resp;
	gnutls_datum_t resp_data; /* DER encoded */

	GArray* certificates;
} ocsp_response;

struct liGnuTLSOCSP {
	GArray* responses;
	size_t max_serial_length;
	size_t max_hash_length;
};

static void clear_entry(ocsp_response_cert_entry* entry) {
	gnutls_free(entry->issuer_name_hash.data);
	gnutls_free(entry->serial.data);
	memset(entry, 0, sizeof(*entry));
}

static int get_entry(liServer *srv, ocsp_response_cert_entry* entry, gnutls_ocsp_resp_t resp, unsigned int ndx) {
	int r;
	memset(entry, 0, sizeof(*entry));

	r = gnutls_ocsp_resp_get_single(
			resp, ndx, &entry->digest, &entry->issuer_name_hash, NULL, &entry->serial,
			NULL, NULL, NULL, NULL, NULL);

	if (GNUTLS_E_REQUESTED_DATA_NOT_AVAILABLE == r) return r;

	if (GNUTLS_E_SUCCESS > r) {
		ERROR(srv, "Couldn't retrieve OCSP response information for entry %u (%s): %s",
				ndx,
				gnutls_strerror_name(r), gnutls_strerror(r));
		return r;
	}

	if (0 == entry->serial.size || GNUTLS_DIG_UNKNOWN == entry->digest || entry->issuer_name_hash.size != gnutls_hash_get_len(entry->digest)) {
		ERROR(srv, "Invalid OCSP response data in entry %u", ndx);
		return GNUTLS_E_OCSP_RESPONSE_ERROR;
	}

	return GNUTLS_E_SUCCESS;
}

static void clear_response(ocsp_response* response) {
	gnutls_free(response->resp_data.data);
	gnutls_ocsp_resp_deinit(response->resp);
	if (NULL != response->certificates) {
		guint i;
		for (i = 0; i < response->certificates->len; ++i) {
			ocsp_response_cert_entry* entry = &g_array_index(response->certificates, ocsp_response_cert_entry, i);
			clear_entry(entry);
		}
		g_array_free(response->certificates, TRUE);
	}
	memset(response, 0, sizeof(*response));
}

/* pass ownership of der_data */
static gboolean add_response(liServer *srv, liGnuTLSOCSP *ocsp, gnutls_datum_t* der_data) {
	ocsp_response response;
	int r;
	guint i;

	memset(&response, 0, sizeof(response));

	response.resp_data = *der_data;
	der_data->data = NULL; der_data->size = 0;

	if (GNUTLS_E_SUCCESS > (r = gnutls_ocsp_resp_init(&response.resp))) {
		ERROR(srv, "gnutls_ocsp_resp_init (%s): %s",
			gnutls_strerror_name(r), gnutls_strerror(r));
		goto error;
	}

	if (GNUTLS_E_SUCCESS > (r = gnutls_ocsp_resp_import(response.resp, &response.resp_data))) {
		ERROR(srv, "gnutls_ocsp_resp_import (%s): %s",
			gnutls_strerror_name(r), gnutls_strerror(r));
		goto error;
	}

	response.certificates = g_array_new(FALSE, TRUE, sizeof(ocsp_response_cert_entry));
	for (i = 0; ; ++i) {
		ocsp_response_cert_entry entry;

		r = get_entry(srv, &entry, response.resp, i);
		if (GNUTLS_E_REQUESTED_DATA_NOT_AVAILABLE == r) break; /* got them all */
		if (GNUTLS_E_SUCCESS > r) goto error;

		g_array_append_vals(response.certificates, &entry, 1);

		if (entry.serial.size > ocsp->max_serial_length) ocsp->max_serial_length = entry.serial.size;
		if (entry.issuer_name_hash.size > ocsp->max_hash_length) ocsp->max_hash_length = entry.issuer_name_hash.size;
	}

	g_array_append_vals(ocsp->responses, &response, 1);
	return TRUE;

error:
	clear_response(&response);
	return FALSE;
}

static int ctx_ocsp_response(gnutls_session_t session, void* ptr, gnutls_datum_t* ocsp_resp) {
	liGnuTLSOCSP* ocsp = ptr;
	guint i;
	int r;
	gnutls_datum_t serial = { NULL, 0 };
	gnutls_datum_t issuer_name = { NULL, 0 };
	char* issuer_name_hash = NULL;

	if (0 == ocsp->responses->len) return GNUTLS_E_NO_CERTIFICATE_STATUS;

	serial.size = ocsp->max_serial_length;
	serial.data = gnutls_malloc(serial.size);
	issuer_name_hash = gnutls_malloc(ocsp->max_hash_length);

	{
		gnutls_datum_t const* crt_datum;
		gnutls_x509_crt_t crt = NULL;
		size_t serial_size = ocsp->max_serial_length;

		crt_datum = gnutls_certificate_get_ours(session); /* memory is NOT owned */
		if (GNUTLS_E_SUCCESS > (r = gnutls_x509_crt_init(&crt))) goto cleanup;
		if (GNUTLS_E_SUCCESS > (r = gnutls_x509_crt_import(crt, crt_datum, GNUTLS_X509_FMT_DER))) {
			gnutls_x509_crt_deinit(crt);
			goto cleanup;
		}

		;
		if (GNUTLS_E_SUCCESS > (r = gnutls_x509_crt_get_serial(crt, serial.data, &serial_size))) {
			gnutls_x509_crt_deinit(crt);
			goto cleanup;
		}
		serial.size = serial_size;

		if (GNUTLS_E_SUCCESS > (r = gnutls_x509_crt_get_raw_issuer_dn(crt, &issuer_name))) {
			gnutls_x509_crt_deinit(crt);
			goto cleanup;
		}

		gnutls_x509_crt_deinit(crt);
	}

	for (i = 0; i < ocsp->responses->len; ++i) {
		ocsp_response* response = &g_array_index(ocsp->responses, ocsp_response, i);
		guint j;

		for (j = 0; j < response->certificates->len; ++j) {
			ocsp_response_cert_entry* entry = &g_array_index(response->certificates, ocsp_response_cert_entry, i);

			if (serial.size != entry->serial.size
					|| 0 != memcmp(serial.data, entry->serial.data, serial.size)) continue;

			if (GNUTLS_E_SUCCESS > (r = gnutls_hash_fast(entry->digest, issuer_name.data, issuer_name.size, issuer_name_hash))) goto cleanup;

			if (0 != memcmp(issuer_name_hash, entry->issuer_name_hash.data, entry->issuer_name_hash.size)) continue;

			ocsp_resp->size = response->resp_data.size;
			ocsp_resp->data = gnutls_malloc(ocsp_resp->size);
			memcpy(ocsp_resp->data, response->resp_data.data, ocsp_resp->size);
			r = GNUTLS_E_SUCCESS;
			goto cleanup;
		}
	}

	r = GNUTLS_E_NO_CERTIFICATE_STATUS;

cleanup:
	gnutls_free(issuer_name_hash);
	gnutls_free(issuer_name.data);
	gnutls_free(serial.data);

	return r;
}

liGnuTLSOCSP* li_gnutls_ocsp_new(void) {
	liGnuTLSOCSP* ocsp = g_slice_new0(liGnuTLSOCSP);
	ocsp->responses = g_array_new(FALSE, TRUE, sizeof(ocsp_response));
	ocsp->max_hash_length = ocsp->max_serial_length = 0;
	return ocsp;
}

void li_gnutls_ocsp_free(liGnuTLSOCSP *ocsp) {
	if (NULL != ocsp->responses) {
		guint i;
		for (i = 0; i < ocsp->responses->len; ++i) {
			ocsp_response* response = &g_array_index(ocsp->responses, ocsp_response, i);
			clear_response(response);
		}
		g_array_free(ocsp->responses, TRUE);
	}
	memset(ocsp, 0, sizeof(*ocsp));
	g_slice_free(liGnuTLSOCSP, ocsp);
}

void li_gnutls_ocsp_use(liGnuTLSOCSP *ocsp, gnutls_certificate_credentials_t creds) {
	gnutls_certificate_set_ocsp_status_request_function(creds, ctx_ocsp_response, ocsp);
}

gboolean li_gnutls_ocsp_add(liServer *srv, liGnuTLSOCSP *ocsp, const char* filename) {
	int r;
	gnutls_datum_t file = { NULL, 0 };
	gnutls_datum_t decoded = { NULL, 0 };
	gnutls_datum_t* der_data;
	gboolean result = FALSE;

	if (GNUTLS_E_SUCCESS > (r = gnutls_load_file(filename, &file))) {
		ERROR(srv, "Failed to load OCSP file '%s' (%s): %s",
				filename,
				gnutls_strerror_name(r), gnutls_strerror(r));
		goto error;
	}

	/* decode pem "-----BEGIN OCSP RESPONSE-----", otherwise expect DER */
	if (file.size > 20 && 0 == memcmp(file.data, CONST_STR_LEN("-----BEGIN "))) {
		r = gnutls_pem_base64_decode_alloc("OCSP RESPONSE", &file, &decoded);

		if (GNUTLS_E_SUCCESS > r) {
			ERROR(srv, "gnutls_pem_base64_decode_alloc failed to decode OCSP RESPONSE from '%s' (%s): %s",
				filename,
				gnutls_strerror_name(r), gnutls_strerror(r));
			goto error;
		}
		der_data = &decoded;
	} else {
		der_data = &file;
	}

	result = add_response(srv, ocsp, der_data);
	if (!result) {
		ERROR(srv, "Failed loading OCSP response from '%s'", filename);
	}

error:
	gnutls_free(file.data);
	gnutls_free(decoded.data);
	return result;
}

gboolean li_gnutls_ocsp_search(liServer *srv, liGnuTLSOCSP *ocsp, const char* filename) {
	int r;
	gnutls_datum_t file = { NULL, 0 };
	gnutls_datum_t decoded = { NULL, 0 };
	gboolean result = FALSE;

	if (GNUTLS_E_SUCCESS > (r = gnutls_load_file(filename, &file))) {
		ERROR(srv, "Failed to load OCSP file '%s' (%s): %s",
				filename,
				gnutls_strerror_name(r), gnutls_strerror(r));
		goto error;
	}

	r = gnutls_pem_base64_decode_alloc("OCSP RESPONSE", &file, &decoded);

	if (GNUTLS_E_SUCCESS <= r) {
		result = add_response(srv, ocsp, &decoded);
		if (!result) {
			ERROR(srv, "Failed loading OCSP response from '%s'", filename);
			goto error;
		}
	} else if (GNUTLS_E_BASE64_UNEXPECTED_HEADER_ERROR == r) {
		/* ignore GNUTLS_E_BASE64_UNEXPECTED_HEADER_ERROR */
	} else {
		ERROR(srv, "gnutls_pem_base64_decode_alloc failed to decode OCSP RESPONSE from '%s' (%s): %s",
			filename,
			gnutls_strerror_name(r), gnutls_strerror(r));
		/* continue anyway */
	}
	result = TRUE;

error:
	gnutls_free(file.data);
	gnutls_free(decoded.data);
	return result;
}

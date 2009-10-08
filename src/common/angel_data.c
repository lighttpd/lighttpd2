
#include <lighttpd/settings.h>
#include <lighttpd/angel_data.h>
#include <lighttpd/utils.h>

/* error handling */
GQuark li_angel_data_error_quark() {
	return g_quark_from_static_string("angel-data-error-quark");
}

static gboolean error_eof(GError **err, const gchar *info) {
	g_set_error(err,
		LI_ANGEL_DATA_ERROR,
		LI_ANGEL_DATA_ERROR_EOF,
		"Not enough data to read value '%s'", info);
	return FALSE;
}

/* write */

gboolean li_angel_data_write_int32(GString *buf, gint32 i, GError **err) {
	g_return_val_if_fail(err == NULL || *err == NULL, FALSE);
	g_string_append_len(buf, (const gchar*) &i, sizeof(i));
	return TRUE;
}

gboolean li_angel_data_write_int64(GString *buf, gint64 i, GError **err) {
	g_return_val_if_fail(err == NULL || *err == NULL, FALSE);
	g_string_append_len(buf, (const gchar*) &i, sizeof(i));
	return TRUE;
}

gboolean li_angel_data_write_char (GString *buf, gchar c, GError **err) {
	g_return_val_if_fail(err == NULL || *err == NULL, FALSE);
	g_string_append_len(buf, &c, sizeof(c));
	return TRUE;
}

gboolean li_angel_data_write_str  (GString *buf, const GString *str, GError **err) {
	g_return_val_if_fail(err == NULL || *err == NULL, FALSE);
	if (str->len > LI_ANGEL_DATA_MAX_STR_LEN) {
		g_set_error(err,
			LI_ANGEL_DATA_ERROR,
			LI_ANGEL_DATA_ERROR_STRING_TOO_LONG,
			"String too long (len: %" G_GSIZE_FORMAT "): '%s'", str->len, str->str);
		return FALSE;
	}
	if (!li_angel_data_write_int32(buf, str->len, err)) return FALSE;
	g_string_append_len(buf, GSTR_LEN(str));
	return TRUE;
}

gboolean li_angel_data_write_cstr (GString *buf, const gchar *str, gsize len, GError **err) {
	const GString tmps = li_const_gstring(str, len); /* fake const GString */
	return li_angel_data_write_str(buf, &tmps, err);
}

/* read */

gboolean li_angel_data_read_int32(liAngelBuffer *buf, gint32 *val, GError **err) {
	g_return_val_if_fail(err == NULL || *err == NULL, FALSE);
	if (buf->data->len - buf->pos < sizeof(gint32)) {
		return error_eof(err, "int32");
	}
	if (val) {
		memcpy(val, buf->data->str + buf->pos, sizeof(gint32));
	}
	buf->pos += sizeof(gint32);
	return TRUE;
}

gboolean li_angel_data_read_int64(liAngelBuffer *buf, gint64 *val, GError **err) {
	g_return_val_if_fail(err == NULL || *err == NULL, FALSE);
	if (buf->data->len - buf->pos < sizeof(gint64)) {
		return error_eof(err, "int64");
	}
	if (val) {
		memcpy(val, buf->data->str + buf->pos, sizeof(gint64));
	}
	buf->pos += sizeof(gint64);
	return TRUE;
}

gboolean li_angel_data_read_char (liAngelBuffer *buf, gchar *val, GError **err) {
	g_return_val_if_fail(err == NULL || *err == NULL, FALSE);
	if (buf->data->len - buf->pos < sizeof(gchar)) {
		return error_eof(err, "char");
	}
	if (val) {
		*val = buf->data->str[buf->pos];
	}
	buf->pos += sizeof(gchar);
	return TRUE;
}

gboolean li_angel_data_read_mem  (liAngelBuffer *buf, GString **val, gsize len, GError **err) {
	GString *s;
	g_return_val_if_fail(err == NULL || *err == NULL, FALSE);

	if (buf->data->len - buf->pos < len) {
		return error_eof(err, "string-data");
	}
	s = *val;
	if (!s) {
		*val = s = g_string_sized_new(len);
	} else {
		g_string_truncate(s, 0);
	}
	g_string_append_len(s, buf->data->str + buf->pos, len);
	buf->pos += len;
	return TRUE;
}

gboolean li_angel_data_read_str  (liAngelBuffer *buf, GString **val, GError **err) {
	gint32 ilen;
	g_return_val_if_fail(err == NULL || *err == NULL, FALSE);

	if (buf->data->len - buf->pos < sizeof(gint32)) {
		return error_eof(err, "string-length");
	}
	memcpy(&ilen, buf->data->str + buf->pos, sizeof(ilen));
	buf->pos += sizeof(gint32);
	if (ilen < 0 || ilen > LI_ANGEL_DATA_MAX_STR_LEN) {
		buf->pos -= sizeof(gint32);
		g_set_error(err,
			LI_ANGEL_DATA_ERROR,
			LI_ANGEL_DATA_ERROR_INVALID_STRING_LENGTH,
			"String length in buffer invalid: %i", (gint) ilen);
		return FALSE;
	}
	if (!li_angel_data_read_mem(buf, val, (gsize) ilen, err)) {
		buf->pos -= sizeof(gint32);
		return FALSE;
	}
	return TRUE;
}

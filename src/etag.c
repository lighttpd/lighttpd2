
#include <lighttpd/base.h>
#include <lighttpd/plugin_core.h>

tristate_t http_response_handle_cachable_etag(vrequest *vr, GString *etag) {
	GList *l;
	tristate_t res = TRI_MAYBE;
	gchar *setag;

	if (!etag) {
		http_header *hetag = http_header_lookup(vr->response.headers, CONST_STR_LEN("etag"));
		if (hetag) setag = hetag->data->str + hetag->keylen + 2;
	} else {
		setag = etag->str;
	}

	for (
			l = http_header_find_first(vr->request.headers, CONST_STR_LEN("If-None-Match"));
			l;
			l = http_header_find_next(l, CONST_STR_LEN("If-None-Match"))) {
		http_header *h = (http_header*) l->data;
		res = TRI_FALSE; /* if the header was given at least once, we need a match */
		if (!setag) return res;
		if (strstr(h->data->str + h->keylen + 2, setag)) {
			return TRI_TRUE;
		}
	}
	return res;
}

tristate_t http_response_handle_cachable_modified(vrequest *vr, GString *last_modified) {
	GList *l;
	tristate_t res = TRI_MAYBE;
	gchar *slm, *hlm;
	http_header *h;
	size_t used_len;
	char *semicolon;

	if (!last_modified) {
		h = http_header_lookup(vr->response.headers, CONST_STR_LEN("last-modified"));
		if (h) slm = h->data->str + h->keylen + 2;
	} else {
		slm = last_modified->str;
	}

	l = http_header_find_first(vr->request.headers, CONST_STR_LEN("If-Modified-Since"));
	if (!l) return TRI_MAYBE;
	if (http_header_find_next(l, CONST_STR_LEN("If-Modified-Since"))) {
		return TRI_FALSE;
	}
	h = (http_header*) l->data;
	hlm = h->data->str + h->keylen + 2;
	if (!slm) return res;

	if (NULL == (semicolon = strchr(hlm, ';'))) {
		used_len = strlen(hlm);
	} else {
		used_len = semicolon - hlm;
	}

	if (0 == strncmp(hlm, slm, used_len)) {
		return TRI_TRUE;
	} else {
		char buf[sizeof("Sat, 23 Jul 2005 21:20:01 GMT")];
		time_t t_header, t_file;
		struct tm tm;
	
		/* check if we can safely copy the string */
		if (used_len >= sizeof(buf)) {
			if (CORE_OPTION(CORE_OPTION_DEBUG_REQUEST_HANDLING).boolean) {
				VR_DEBUG(vr, "Last-Modified check failed as the received timestamp '%s' was too long (%u > %u)",
					hlm, (int) used_len, (int) sizeof(buf) - 1);
			}
			/* not returning "412" - should we? */
			return TRI_FALSE;
		}

		strncpy(buf, hlm, used_len);
		buf[used_len] = '\0';

		memset(&tm, 0, sizeof(tm));
		if (NULL == strptime(buf, "%a, %d %b %Y %H:%M:%S GMT", &tm)) {
			/* not returning "412" - should we? */
			return TRI_FALSE;
		}
		t_header = mktime(&tm);

		memset(&tm, 0, sizeof(tm));
		strptime(slm, "%a, %d %b %Y %H:%M:%S GMT", &tm);
		t_file = mktime(&tm);

		if (t_file > t_header) return TRI_FALSE;

		return TRI_TRUE;
	}
}

void etag_mutate(GString *mut, GString *etag) {
	guint i;
	guint32 h;

	for (h=0, i=0; i < etag->len; ++i) h = (h<<5)^(h>>27)^(etag->str[i]);

	g_string_truncate(mut, 0);
	g_string_append_len(mut, CONST_STR_LEN("\""));
	l_g_string_append_int(mut, (guint64) h);
	g_string_append_len(mut, CONST_STR_LEN("\""));
}

void etag_set_header(vrequest *vr, struct stat *st, gboolean *cachable) {
	guint flags = CORE_OPTION(CORE_OPTION_ETAG_FLAGS).number;
	GString *tmp_str = vr->con->wrk->tmp_str;
	struct tm tm;
	tristate_t c_able = cachable ? TRI_MAYBE : TRI_FALSE;

	if (0 == flags) {
		http_header_remove(vr->response.headers, CONST_STR_LEN("etag"));
	} else {
		g_string_truncate(tmp_str, 0);
	
		if (flags & ETAG_USE_INODE) {
			l_g_string_append_int(tmp_str, st->st_ino);
		}
		
		if (flags & ETAG_USE_SIZE) {
			if (tmp_str->len != 0) g_string_append_len(tmp_str, CONST_STR_LEN("-"));
			l_g_string_append_int(tmp_str, st->st_size);
		}
		
		if (flags & ETAG_USE_MTIME) {
			if (tmp_str->len != 0) g_string_append_len(tmp_str, CONST_STR_LEN("-"));
			l_g_string_append_int(tmp_str, st->st_mtime);
		}
	
		etag_mutate(tmp_str, tmp_str);
	
		http_header_overwrite(vr->response.headers, CONST_STR_LEN("ETag"), GSTR_LEN(tmp_str));

		if (c_able != TRI_FALSE) {
			switch (http_response_handle_cachable_etag(vr, tmp_str)) {
			case TRI_FALSE: c_able = TRI_FALSE; break;
			case TRI_MAYBE: break;
			case TRI_TRUE : c_able = TRI_TRUE; break;
			}
		}
	}

	if (gmtime_r(&st->st_mtime, &tm)) {
		g_string_set_size(tmp_str, 256);
		g_string_set_size(tmp_str, strftime(tmp_str->str, tmp_str->len-1,
			"%a, %d %b %Y %H:%M:%S GMT", &tm));
		http_header_overwrite(vr->response.headers, CONST_STR_LEN("Last-Modified"), GSTR_LEN(tmp_str));

		if (c_able != TRI_FALSE) {
			switch (http_response_handle_cachable_modified(vr, tmp_str)) {
			case TRI_FALSE: c_able = TRI_FALSE; break;
			case TRI_MAYBE: break;
			case TRI_TRUE : c_able = TRI_TRUE; break;
			}
		}
	}

	if (cachable) *cachable = (c_able == TRI_TRUE);
}

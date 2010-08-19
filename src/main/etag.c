
#include <lighttpd/base.h>
#include <lighttpd/plugin_core.h>

liTristate li_http_response_handle_cachable_etag(liVRequest *vr, GString *etag) {
	GList *l;
	liTristate res = LI_TRIMAYBE;
	gchar *setag = NULL;

	if (!etag) {
		liHttpHeader *hetag = li_http_header_lookup(vr->response.headers, CONST_STR_LEN("etag"));
		if (hetag) setag = hetag->data->str + hetag->keylen + 2;
	} else {
		setag = etag->str;
	}

	for (
			l = li_http_header_find_first(vr->request.headers, CONST_STR_LEN("If-None-Match"));
			l;
			l = li_http_header_find_next(l, CONST_STR_LEN("If-None-Match"))) {
		liHttpHeader *h = (liHttpHeader*) l->data;
		res = LI_TRIFALSE; /* if the header was given at least once, we need a match */
		if (!setag) return res;
		if (strstr(h->data->str + h->keylen + 2, setag)) {
			return LI_TRITRUE;
		}
	}
	return res;
}

liTristate li_http_response_handle_cachable_modified(liVRequest *vr, GString *last_modified) {
	GList *l;
	gchar *slm = NULL, *hlm;
	liHttpHeader *h;
	size_t used_len;
	char *semicolon;

	if (!last_modified) {
		h = li_http_header_lookup(vr->response.headers, CONST_STR_LEN("last-modified"));
		if (h) slm = h->data->str + h->keylen + 2;
	} else {
		slm = last_modified->str;
	}

	l = li_http_header_find_first(vr->request.headers, CONST_STR_LEN("If-Modified-Since"));
	if (!l) return LI_TRIMAYBE; /* no if-modified-since header */
	if (li_http_header_find_next(l, CONST_STR_LEN("If-Modified-Since"))) {
		return LI_TRIFALSE; /* we only check one if-modified-since header */
	}
	h = (liHttpHeader*) l->data;
	hlm = h->data->str + h->keylen + 2;
	if (!slm) return LI_TRIFALSE;

	if (NULL == (semicolon = strchr(hlm, ';'))) {
		used_len = strlen(hlm);
	} else {
		used_len = semicolon - hlm;
	}

	if (0 == strncmp(hlm, slm, used_len)) {
		return (slm[used_len] == '\0' || slm[used_len] == ';') ? LI_TRITRUE : LI_TRIFALSE;
	} else {
		char buf[sizeof("Sat, 23 Jul 2005 21:20:01 GMT")];
		time_t t_header, t_file;
		struct tm tm;
	
		/* check if we can safely copy the string */
		if (used_len >= sizeof(buf)) {
			if (CORE_OPTION(LI_CORE_OPTION_DEBUG_REQUEST_HANDLING).boolean) {
				VR_DEBUG(vr, "Last-Modified check failed as the received timestamp '%s' was too long (%u > %u)",
					hlm, (int) used_len, (int) sizeof(buf) - 1);
			}
			/* not returning "412" - should we? */
			return LI_TRIFALSE;
		}

		strncpy(buf, hlm, used_len);
		buf[used_len] = '\0';

		memset(&tm, 0, sizeof(tm));
		if (NULL == strptime(buf, "%a, %d %b %Y %H:%M:%S GMT", &tm)) {
			/* not returning "412" - should we? */
			return LI_TRIFALSE;
		}
		tm.tm_isdst = 0;
		t_header = mktime(&tm);

		memset(&tm, 0, sizeof(tm));
		strptime(slm, "%a, %d %b %Y %H:%M:%S GMT", &tm);
		tm.tm_isdst = 0;
		t_file = mktime(&tm);

		if (t_file > t_header) return LI_TRIFALSE;

		return LI_TRITRUE;
	}
}

gboolean li_http_response_handle_cachable(liVRequest *vr) {
	liTristate c_able = LI_TRIMAYBE;
	if (c_able != LI_TRIFALSE) {
		switch (li_http_response_handle_cachable_etag(vr, NULL)) {
		case LI_TRIFALSE: c_able = LI_TRIFALSE; break;
		case LI_TRIMAYBE: break;
		case LI_TRITRUE : c_able = LI_TRITRUE; break;
		}
	}
	if (c_able != LI_TRIFALSE) {
		switch (li_http_response_handle_cachable_modified(vr, NULL)) {
		case LI_TRIFALSE: c_able = LI_TRIFALSE; break;
		case LI_TRIMAYBE: break;
		case LI_TRITRUE : c_able = LI_TRITRUE; break;
		}
	}

	return c_able == LI_TRITRUE;
}

void li_etag_mutate(GString *mut, GString *etag) {
	guint i;
	guint32 h;

	for (h=0, i=0; i < etag->len; ++i) h = (h<<5)^(h>>27)^(etag->str[i]);

	g_string_truncate(mut, 0);
	g_string_append_len(mut, CONST_STR_LEN("\""));
	li_string_append_int(mut, (guint64) h);
	g_string_append_len(mut, CONST_STR_LEN("\""));
}

void li_etag_set_header(liVRequest *vr, struct stat *st, gboolean *cachable) {
	guint flags = CORE_OPTION(LI_CORE_OPTION_ETAG_FLAGS).number;
	GString *tmp_str = vr->wrk->tmp_str;
	struct tm tm;
	liTristate c_able = cachable ? LI_TRIMAYBE : LI_TRIFALSE;

	if (0 == flags) {
		li_http_header_remove(vr->response.headers, CONST_STR_LEN("etag"));
	} else {
		g_string_truncate(tmp_str, 0);
	
		if (flags & LI_ETAG_USE_INODE) {
			li_string_append_int(tmp_str, st->st_ino);
		}
		
		if (flags & LI_ETAG_USE_SIZE) {
			if (tmp_str->len != 0) g_string_append_len(tmp_str, CONST_STR_LEN("-"));
			li_string_append_int(tmp_str, st->st_size);
		}
		
		if (flags & LI_ETAG_USE_MTIME) {
			if (tmp_str->len != 0) g_string_append_len(tmp_str, CONST_STR_LEN("-"));
			li_string_append_int(tmp_str, st->st_mtime);
		}
	
		li_etag_mutate(tmp_str, tmp_str);
	
		li_http_header_overwrite(vr->response.headers, CONST_STR_LEN("ETag"), GSTR_LEN(tmp_str));

		if (c_able != LI_TRIFALSE) {
			switch (li_http_response_handle_cachable_etag(vr, tmp_str)) {
			case LI_TRIFALSE: c_able = LI_TRIFALSE; break;
			case LI_TRIMAYBE: break;
			case LI_TRITRUE : c_able = LI_TRITRUE; break;
			}
		}
	}

	if (gmtime_r(&st->st_mtime, &tm)) {
		g_string_set_size(tmp_str, 256);
		g_string_set_size(tmp_str, strftime(tmp_str->str, tmp_str->len-1,
			"%a, %d %b %Y %H:%M:%S GMT", &tm));
		li_http_header_overwrite(vr->response.headers, CONST_STR_LEN("Last-Modified"), GSTR_LEN(tmp_str));

		if (c_able != LI_TRIFALSE) {
			switch (li_http_response_handle_cachable_modified(vr, tmp_str)) {
			case LI_TRIFALSE: c_able = LI_TRIFALSE; break;
			case LI_TRIMAYBE: break;
			case LI_TRITRUE : c_able = LI_TRITRUE; break;
			}
		}
	}

	if (cachable) *cachable = (c_able == LI_TRITRUE);
}
